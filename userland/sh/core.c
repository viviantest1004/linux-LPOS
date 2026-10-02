/* core.c - memory, strings, buffered output, messages and the kernel
 * calls the shared libc does not wrap. Part of sh.c; see sh.h.
 *
 * ── Why the shell has its own allocator ──
 *
 * The libc malloc is a first-fit walk over every block the heap has ever
 * held, free or not. That is the right size of thing for a program that
 * allocates a few dozen buffers and exits. A shell is the opposite: it
 * allocates and frees small strings all day - every word of every
 * command, every variable assignment in a loop - and keeps thousands of
 * them alive (variables, functions, history). With first-fit over the
 * whole heap each allocation costs a walk over all of those, so a loop
 * gets slower the longer the shell has been running.
 *
 * So small requests are rounded up to one of a few size classes and kept
 * on a free list per class: allocation and release are a pointer swap.
 * Blocks are carved out of 64KB slabs taken from the libc allocator,
 * which therefore sees a handful of large blocks and stays fast too.
 * Large requests go straight to libc. Nothing is ever handed back to the
 * kernel; a shell's high-water mark is small and is reached early. */

#define NCLASS 14
static const u32 class_size[NCLASS] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
};
#define CLASS_LARGE 0xFFu
#define SLAB_SIZE   (64u * 1024u)

/* 16 bytes on every machine, so what follows it is 16-byte aligned. */
typedef union {
    struct {
        u32    cls;
        u32    pad;
        size_t size;        /* usable bytes of a large block */
    } h;
    u8 raw[16];
} mhdr_t;

typedef struct fblk { struct fblk *next; } fblk_t;

static fblk_t *freelist[NCLASS];
static char   *slab_p;
static size_t  slab_left;

static void out_of_memory(void) __attribute__((noreturn));
static void out_of_memory(void)
{
    static const char msg[] = "sh: out of memory\n";
    lp_write(2, msg, sizeof msg - 1);
    lp_exit(2);
}

static void *xmalloc(size_t n)
{
    if (n == 0)
        n = 1;
    for (u32 c = 0; c < NCLASS; c++) {
        if (n > class_size[c])
            continue;
        mhdr_t *h;
        if (freelist[c]) {
            h = (mhdr_t *)(void *)freelist[c];
            freelist[c] = freelist[c]->next;
        } else {
            size_t need = sizeof(mhdr_t) + class_size[c];
            if (slab_left < need) {
                slab_p = malloc(SLAB_SIZE);
                if (!slab_p)
                    out_of_memory();
                slab_left = SLAB_SIZE;
            }
            h = (mhdr_t *)(void *)slab_p;
            slab_p += need;
            slab_left -= need;
        }
        h->h.cls = c;
        return h + 1;
    }
    mhdr_t *h = malloc(sizeof(mhdr_t) + n);
    if (!h)
        out_of_memory();
    h->h.cls = CLASS_LARGE;
    h->h.size = n;
    return h + 1;
}

static void xfree(void *p)
{
    if (!p)
        return;
    mhdr_t *h = (mhdr_t *)p - 1;
    if (h->h.cls == CLASS_LARGE) {
        free(h);
        return;
    }
    fblk_t *f = (fblk_t *)(void *)h;
    f->next = freelist[h->h.cls];
    freelist[h->h.cls] = f;
}

static size_t xusable(void *p)
{
    mhdr_t *h = (mhdr_t *)p - 1;
    return h->h.cls == CLASS_LARGE ? h->h.size : class_size[h->h.cls];
}

static void *xrealloc(void *p, size_t n)
{
    if (!p)
        return xmalloc(n);
    size_t have = xusable(p);
    if (n <= have)
        return p;
    void *q = xmalloc(n);
    memcpy(q, p, have);
    xfree(p);
    return q;
}

static void *xcalloc(size_t n)
{
    void *p = xmalloc(n);
    memset(p, 0, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

/* ── strbuf ────────────────────────────────────────────────────────── */

static void sb_grow(strbuf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < b->len + need + 1)
        cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

static void sb_putc(strbuf_t *b, char c)
{
    sb_grow(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

static void sb_putn(strbuf_t *b, const char *s, size_t n)
{
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_puts(strbuf_t *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

static char *sb_str(strbuf_t *b)
{
    if (!b->s)
        sb_grow(b, 0), b->s[0] = '\0';
    return b->s;
}

static char *sb_take(strbuf_t *b)
{
    char *s = sb_str(b);
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

static void sb_free(strbuf_t *b)
{
    xfree(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

/* The formatter behind every message. The libc has snprintf but no
 * va_list form of it, and messages need one. It knows the conversions
 * the shell uses and nothing more: %s %c %d %i %u %x %o %%, with the l,
 * ll and z sizes, a width, '-' and '0', and a precision for %s. */
static void sb_vprintf(strbuf_t *b, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            const char *e = fmt;
            while (*e && *e != '%')
                e++;
            sb_putn(b, fmt, (size_t)(e - fmt));
            fmt = e;
            continue;
        }
        fmt++;
        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (is_digit(*fmt))
                width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                prec = 0;
                while (is_digit(*fmt))
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = sizeof(size_t) == 8 ? 2 : 0; fmt++; }
        if (lng == 1 && sizeof(long) == 8) lng = 2;

        char num[32];
        const char *str = num;
        size_t slen = 0;
        char conv = *fmt ? *fmt++ : '\0';
        switch (conv) {
        case 's':
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            slen = strlen(str);
            if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
            break;
        case 'c':
            num[0] = (char)va_arg(ap, int);
            slen = 1;
            break;
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long)
                        : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            itoa_s(v, num);
            slen = strlen(num);
            break;
        }
        case 'u': case 'x': case 'o': {
            unsigned long long v = lng == 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned);
            unsigned base = conv == 'u' ? 10 : conv == 'x' ? 16 : 8;
            char tmp[32];
            int n = 0;
            do {
                tmp[n++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v);
            for (int i = 0; i < n; i++)
                num[i] = tmp[n - 1 - i];
            slen = (size_t)n;
            break;
        }
        case '%':
            num[0] = '%';
            slen = 1;
            break;
        default:
            continue;
        }
        size_t pad = (size_t)width > slen ? (size_t)width - slen : 0;
        if (!left)
            for (size_t i = 0; i < pad; i++)
                sb_putc(b, zero && conv != 's' ? '0' : ' ');
        sb_putn(b, str, slen);
        if (left)
            for (size_t i = 0; i < pad; i++)
                sb_putc(b, ' ');
    }
    sb_str(b);
}

static void sb_printf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

/* ── strvec ────────────────────────────────────────────────────────── */

static void sv_push(strvec_t *v, char *s)
{
    if (v->n + 2 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = xrealloc(v->v, (size_t)v->cap * sizeof(char *));
    }
    v->v[v->n++] = s;
    v->v[v->n] = NULL;
}

static void sv_free(strvec_t *v)
{
    for (int i = 0; i < v->n; i++)
        xfree(v->v[i]);
    xfree(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

/* ── output ────────────────────────────────────────────────────────── */

static void flush_out(out_t *o)
{
    if (o->n == 0)
        return;
    size_t off = 0;
    while (off < o->n) {
        long w = lp_write(o->fd, o->buf + off, o->n - off);
        if (w == -4)            /* EINTR */
            continue;
        if (w <= 0) {
            o->err = true;
            o->errnum = w < 0 ? (int)-w : 5;
            break;
        }
        off += (size_t)w;
    }
    o->n = 0;
}

static void flush_all(void)
{
    flush_out(out1);
    flush_out(out2);
}

static void outn(out_t *o, const char *s, size_t n)
{
    while (n) {
        if (o->n == sizeof o->buf)
            flush_out(o);
        size_t room = sizeof o->buf - o->n;
        size_t k = n < room ? n : room;
        memcpy(o->buf + o->n, s, k);
        o->n += k;
        s += k;
        n -= k;
    }
}

static void outs(out_t *o, const char *s) { outn(o, s, strlen(s)); }
static void outc(out_t *o, char c)        { outn(o, &c, 1); }

static void outf(out_t *o, const char *fmt, ...)
{
    strbuf_t b = {0};
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    outn(o, b.s, b.len);
    sb_free(&b);
}

/* ── messages ──────────────────────────────────────────────────────── */

static void msg_prefix(strbuf_t *b)
{
    if (toplevel_interactive || !script_name) {
        sb_puts(b, "sh: ");
        return;
    }
    int line = cmd_lineno > 0 ? cmd_lineno : input_lineno();
    if (line > 0)
        sb_printf(b, "%s: line %d: ", script_name, line);
    else
        sb_printf(b, "%s: ", script_name);
}

static void sh_vwarn(const char *fmt, va_list ap)
{
    strbuf_t b = {0};
    msg_prefix(&b);
    sb_vprintf(&b, fmt, ap);
    sb_putc(&b, '\n');
    flush_out(out1);
    flush_out(out2);
    xwrite_all(2, b.s, b.len);
    sb_free(&b);
}

static void sh_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sh_vwarn(fmt, ap);
    va_end(ap);
}

static void sh_error(int status, const char *fmt, ...)
{
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        sh_vwarn(fmt, ap);
        va_end(ap);
    }
    exitstatus = status;
    if (!toplevel_interactive)
        exitshell(status);
    evalskip = SKIP_ABORT;
}

static void sh_perror(const char *what, long err)
{
    sh_warn("%s: %s", what, lp_strerror((int)(err < 0 ? -err : err)));
}

/* ── small helpers ─────────────────────────────────────────────────── */

static bool is_digit(int c)      { return c >= '0' && c <= '9'; }
static bool is_blank(int c)      { return c == ' ' || c == '\t'; }
static bool is_name_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_name_char(int c)  { return is_name_start(c) || is_digit(c); }

static bool is_valid_name(const char *s)
{
    if (!is_name_start((unsigned char)*s))
        return false;
    while (*++s)
        if (!is_name_char((unsigned char)*s))
            return false;
    return true;
}

/* A decimal integer with optional sign and surrounding blanks, which is
 * what `test 3 -eq " 3 "` and `exit 2` both accept. False on anything
 * else, including overflow. */
static bool parse_int(const char *s, long long *out)
{
    while (is_blank(*s) || *s == '\n') s++;
    bool neg = false;
    if (*s == '+' || *s == '-')
        neg = *s++ == '-';
    if (!is_digit(*s))
        return false;
    unsigned long long v = 0;
    while (is_digit(*s)) {
        unsigned long long nv = v * 10 + (unsigned)(*s++ - '0');
        if (nv / 10 != v || nv > 9223372036854775808ULL)
            return false;
        v = nv;
    }
    while (is_blank(*s) || *s == '\n') s++;
    if (*s)
        return false;
    if (!neg && v > 9223372036854775807ULL)
        return false;
    *out = neg ? (long long)(0 - v) : (long long)v;
    return true;
}

static char *itoa_s(long long v, char *buf)
{
    char tmp[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0 - (unsigned long long)v
                                 : (unsigned long long)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    int i = 0;
    if (v < 0)
        buf[i++] = '-';
    while (n)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

/* ── kernel calls ──────────────────────────────────────────────────── *
 *
 * The numbers the shared syscall tables do not carry. On 32-bit ARM the
 * uid calls are the *32 variants: the plain ones are the 16-bit calls
 * from before Linux had 32-bit uids, and would report uid 100000 as
 * 34464. */
#if defined(__x86_64__)
#  define NR_umask        95
#  define NR_times        100
#  define NR_getuid_      102
#  define NR_getgid_      104
#  define NR_geteuid      107
#  define NR_getegid      108
#  define NR_fcntl_       72
#elif defined(__aarch64__)
#  define NR_umask        166
#  define NR_times        153
#  define NR_getuid_      174
#  define NR_geteuid      175
#  define NR_getgid_      176
#  define NR_getegid      177
#  define NR_fcntl_       25
#elif defined(__arm__)
#  define NR_umask        60
#  define NR_times        43
#  define NR_getuid_      199
#  define NR_getgid_      200
#  define NR_geteuid      201
#  define NR_getegid      202
#  define NR_fcntl_       221
#endif

#define F_DUPFD_CLOEXEC 1030
#define F_GETFD_        1
#define TIOCGPGRP_      0x540F
#define TIOCSPGRP_      0x5410

static long sys_umask(long mask)   { return sys_call1(NR_umask, mask); }
static int  sys_geteuid(void)      { return (int)sys_call0(NR_geteuid); }
static int  sys_getegid(void)      { return (int)sys_call0(NR_getegid); }
static int  sys_getgid_(void)      { return (int)sys_call0(NR_getgid_); }
static int  sys_getuid_(void)      { return (int)sys_call0(NR_getuid_); }
static int  sys_getppid(void)      { return (int)sys_call0(SYS_getppid); }
static int  sys_getpgrp(void)      { return (int)sys_call1(SYS_getpgid, 0); }
static int  sys_setpgid(int p, int g)
{
    return (int)sys_call2(SYS_setpgid, p, g);
}

/* times(2) fills four clock_t, which is a long - 4 bytes on the 32-bit
 * board and 8 elsewhere - so the caller's array is of longs too. */
static long sys_times(long *four)  { return sys_call1(NR_times, (long)four); }

static int sys_tcgetpgrp(int fd)
{
    int pg = 0;
    long r = lp_ioctl(fd, TIOCGPGRP_, &pg);
    return r < 0 ? (int)r : pg;
}

static int sys_tcsetpgrp(int fd, int pgid)
{
    int pg = pgid;
    return (int)lp_ioctl(fd, TIOCSPGRP_, &pg);
}

static int fd_dup_high(int fd)
{
    return (int)sys_call3(NR_fcntl_, fd, F_DUPFD_CLOEXEC, 10);
}

static int fd_move_high(int fd)
{
    int n = fd_dup_high(fd);
    if (n >= 0)
        lp_close(fd);
    return n;
}

static bool fd_is_open(int fd)
{
    return sys_call2(NR_fcntl_, fd, F_GETFD_) >= 0;
}

static long sys_sigmask(int how, const u64 *set, u64 *old)
{
    return sys_call4(SYS_rt_sigprocmask, how, (long)set, (long)old, 8);
}

/* What a signal is set to now. The handler field is the first word of
 * the kernel's struct sigaction on every machine we build for, so this
 * needs no per-machine layout - unlike setting one. */
static void sig_disposition(int sig, long *old_handler)
{
    unsigned long buf[8];
    memset(buf, 0, sizeof buf);
    sys_call4(SYS_rt_sigaction, sig, 0, (long)buf, 8);
    *old_handler = (long)buf[0];
}

static long xread(int fd, void *buf, size_t n)
{
    for (;;) {
        long r = lp_read(fd, buf, n);
        if (r != -4)
            return r;
    }
}

static bool xwrite_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        long w = lp_write(fd, p, n);
        if (w == -4)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static s64 now_ms(void) { return lp_monotonic_ms(); }
