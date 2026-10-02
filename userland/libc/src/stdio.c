/* stdio.c - a minimal printf family.
 * No floating point. The shell and init do not need it, and adding it
 * would multiply the code size. */
#include "stdio.h"
#include "string.h"
#include "unistd.h"

#include <stdarg.h>

/* An abstraction over where output goes, so the same formatting code
 * serves both a file descriptor (sink_fd) and a buffer (sink_buf). */
typedef struct {
    int     fd;         /* fd >= 0 goes to a file, otherwise to the buffer */
    char   *buf;
    size_t  cap;        /* size of buf, including room for the NUL */
    size_t  len;        /* length produced so far, counted past truncation */
    char    chunk[256]; /* write buffer, for fd mode */
    size_t  chunk_len;
} sink_t;

static void sink_flush(sink_t *s)
{
    if (s->fd >= 0 && s->chunk_len) {
        lp_write(s->fd, s->chunk, s->chunk_len);
        s->chunk_len = 0;
    }
}

static void sink_put(sink_t *s, char c)
{
    s->len++;

    if (s->fd >= 0) {
        s->chunk[s->chunk_len++] = c;
        if (s->chunk_len == sizeof(s->chunk))
            sink_flush(s);
        return;
    }

    /* Buffer mode: drop the overflow but keep counting, as snprintf does. */
    if (s->buf && s->len <= s->cap - 1)
        s->buf[s->len - 1] = c;
}

/* Render a number into buf and return its length.
 * No padding here - emit_padded handles alignment. */
static unsigned fmt_uint(char *buf, unsigned cap, u64 v, unsigned base, bool upper)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;

    char tmp[24];
    unsigned n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v && n < sizeof(tmp)) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }

    unsigned len = 0;
    while (n && len < cap)
        buf[len++] = tmp[--n];
    return len;
}

/* Emit a field padded to a width.
 *   left  true pads on the right ("%-10s")
 *   pad   the fill character, '0' or ' '
 * Zero padding a signed value has to put the sign first, so the caller
 * passes the sign separately. */
static void emit_padded(sink_t *s, const char *sign, const char *str,
                        unsigned len, unsigned width, bool left, char pad)
{
    unsigned slen = sign ? 1 : 0;
    unsigned total = len + slen;
    unsigned fill  = (width > total) ? width - total : 0;

    if (left) {
        if (sign) sink_put(s, *sign);
        for (unsigned i = 0; i < len; i++) sink_put(s, str[i]);
        while (fill--) sink_put(s, ' ');     /* left aligned always pads with spaces */
        return;
    }

    /* Right aligned. With '0' padding the sign comes before the fill. */
    if (pad == '0' && sign) sink_put(s, *sign);
    while (fill--) sink_put(s, pad);
    if (pad != '0' && sign) sink_put(s, *sign);
    for (unsigned i = 0; i < len; i++) sink_put(s, str[i]);
}

static void emit_uint(sink_t *s, u64 v, unsigned base, bool upper,
                      unsigned width, bool left, bool zero_pad)
{
    char buf[24];
    unsigned len = fmt_uint(buf, sizeof(buf), v, base, upper);
    emit_padded(s, NULL, buf, len, width, left, zero_pad ? '0' : ' ');
}

static void emit_int(sink_t *s, s64 v, unsigned width, bool left, bool zero_pad)
{
    char buf[24];
    const char *sign = NULL;
    u64 mag;

    if (v < 0) {
        sign = "-";
        /* -(-2^63) overflows. Take the sign off first to avoid it. */
        mag = (u64)(-(v + 1)) + 1;
    } else {
        mag = (u64)v;
    }

    unsigned len = fmt_uint(buf, sizeof(buf), mag, 10, false);
    emit_padded(s, sign, buf, len, width, left, zero_pad ? '0' : ' ');
}

static void format(sink_t *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') { sink_put(s, *fmt++); continue; }
        fmt++;

        /* Flags may come in any order; "%-05d" is valid too. */
        bool left_align = false, zero_pad = false;
        for (;;) {
            if (*fmt == '-') { left_align = true; fmt++; continue; }
            if (*fmt == '0') { zero_pad   = true; fmt++; continue; }
            break;
        }

        unsigned width = 0;
        if (*fmt == '*') {
            /* "%*d" takes the width from the argument list. Without this
             * the format was copied out literally and the argument it
             * was meant to consume shifted every later conversion by
             * one - which reads as a crash somewhere else entirely. A
             * negative width means left-aligned, the same as "%-*d". */
            fmt++;
            int w = va_arg(ap, int);
            if (w < 0) { left_align = true; w = -w; }
            width = (unsigned)w;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (unsigned)(*fmt++ - '0');
        }

        /* ".N" or ".*" - a precision. For %s it is a maximum length,
         * which is how you print part of a string that is not NUL
         * terminated where you want it to end: "%.*s" with a length. It
         * was not handled at all, so "dd: no such key: %.*s" printed the
         * format itself and consumed neither argument, and every
         * conversion after it in the same call took the wrong one. -1
         * means "no precision given", which is not the same as 0. */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                fmt++;
                precision = va_arg(ap, int);
                if (precision < 0) precision = -1;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        /* How wide the argument is.
         *
         * This used to be a count that anything above zero treated as
         * 64 bits, which was right on both 64-bit machines and wrong on
         * a 32-bit one: there `%ld` is four bytes and `%lld` is eight,
         * and reading eight for a `%ld` takes the next argument's
         * bytes with it. Every number after the first came out as
         * garbage, which is how `expr 2 + 3` printed 0.
         *
         * `z` is size_t, which is a pointer width - 8 on the 64-bit
         * machines and 4 here. */
        int longness = 0;             /* 0 int, 1 long, 2 long long */
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'z') { longness = (sizeof(size_t) == 8) ? 2 : 1; fmt++; }
        if (longness == 1 && sizeof(long) == 8) longness = 2;
        bool wide = (longness >= 2);

        switch (*fmt) {
        case 'c':
            sink_put(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            unsigned len = (unsigned)strlen(p);
            /* A precision on %s caps the length, and must not read past
             * it either - the string may not be NUL terminated within
             * that many bytes, which is the whole point of using it. */
            if (precision >= 0) {
                unsigned cap = (unsigned)precision;
                len = 0;
                while (len < cap && p[len]) len++;
            }
            emit_padded(s, NULL, p, len, width, left_align, ' ');
            break;
        }
        case 'd': case 'i':
            emit_int(s, wide ? va_arg(ap, s64)
                      : (longness ? (s64)va_arg(ap, long) : (s64)va_arg(ap, s32)),
                     width, left_align, zero_pad);
            break;
        case 'u':
            emit_uint(s, wide ? va_arg(ap, u64)
                       : (longness ? (u64)va_arg(ap, unsigned long)
                                   : (u64)va_arg(ap, u32)),
                      10, false, width, left_align, zero_pad);
            break;
        /* Octal, which exists for exactly one reason on this system:
         * file permissions are read and written in it, and 0755 spelled
         * out in decimal is 493. */
        case 'o':
            emit_uint(s, wide ? va_arg(ap, u64)
                       : (longness ? (u64)va_arg(ap, unsigned long)
                                   : (u64)va_arg(ap, u32)),
                      8, false, width, left_align, zero_pad);
            break;
        case 'x':
            emit_uint(s, wide ? va_arg(ap, u64)
                       : (longness ? (u64)va_arg(ap, unsigned long)
                                   : (u64)va_arg(ap, u32)),
                      16, false, width, left_align, zero_pad);
            break;
        case 'X':
            emit_uint(s, wide ? va_arg(ap, u64)
                       : (longness ? (u64)va_arg(ap, unsigned long)
                                   : (u64)va_arg(ap, u32)),
                      16, true, width, left_align, zero_pad);
            break;
        case 'p':
            sink_put(s, '0'); sink_put(s, 'x');
            emit_uint(s, (u64)(uintptr_t)va_arg(ap, void *), 16, false,
                      16, false, true);
            break;
        case '%':
            sink_put(s, '%');
            break;
        case '\0':
            return;
        default:
            sink_put(s, '%');
            sink_put(s, *fmt);
            break;
        }
        fmt++;
    }
}

static int run_fd(int fd, const char *fmt, va_list ap)
{
    sink_t s = { .fd = fd, .buf = NULL, .cap = 0, .len = 0, .chunk_len = 0 };
    format(&s, fmt, ap);
    sink_flush(&s);
    return (int)s.len;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = run_fd(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = run_fd(fd, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    sink_t s = { .fd = -1, .buf = buf, .cap = size, .len = 0, .chunk_len = 0 };

    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);

    if (buf && size) {
        size_t end = (s.len < size - 1) ? s.len : size - 1;
        buf[end] = '\0';
    }
    return (int)s.len;   /* the full length needed, even when truncated */
}

int fputs(const char *s, int fd)
{
    size_t n = strlen(s);
    return (int)lp_write(fd, s, n);
}

int puts(const char *s)
{
    fputs(s, STDOUT_FILENO);
    lp_write(STDOUT_FILENO, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    lp_write(STDOUT_FILENO, &ch, 1);
    return c;
}

/* readrec - one record ending at `delim`, with the bytes untouched.
 *
 * readline() below is for a terminal: it erases on backspace and always
 * splits on newline. A file tool wants neither. `uniq -z`, `sort -z` and
 * `xargs -0` split on NUL, and a byte that happens to be 0x7F in a file
 * is a byte, not an edit. Returns the length, or -1 at end of input with
 * nothing read; the record is NUL-terminated and the delimiter is not
 * part of it. `saw_delim`, when given, says whether the record ended at
 * a delimiter or at end of input - the difference between a file with a
 * final newline and one without. */
long readrec(int fd, char *buf, size_t size, char delim, bool *saw_delim)
{
    size_t n = 0;
    if (saw_delim) *saw_delim = false;

    for (;;) {
        char c;
        long r = lp_read(fd, &c, 1);

        if (r == 0) {
            buf[n < size ? n : size - 1] = '\0';
            return (n == 0) ? -1 : (long)n;
        }
        if (r < 0)
            return r;
        if (c == delim) {
            if (saw_delim) *saw_delim = true;
            break;
        }
        if (n < size - 1)
            buf[n++] = c;
    }
    buf[n] = '\0';
    return (long)n;
}

long readline(int fd, char *buf, size_t size)
{
    size_t n = 0;
    bool   warned = false;

    for (;;) {
        char c;
        long r = lp_read(fd, &c, 1);

        if (r == 0)                       /* EOF */
            return (n == 0) ? -1 : (long)n;
        if (r < 0)
            return r;

        if (c == '\n')
            break;

        /* Backspace: the bare minimum, for when the kernel is not doing
         *
         * line editing. Erase a character, not a byte: Hangul is 3 bytes,
         * and erasing one would leave a broken fragment in the buffer. */
        if (c == 0x7F || c == '\b') {
            if (n)
                n = utf8_prev(buf, n);
            continue;
        }

        if (n < size - 1) {
            buf[n++] = c;
        } else {
            /* 넘치는 것은 버린다. 예전에는 "부르는 쪽이 길이로 안다"
             * 고 적어 두었는데, 길이가 딱 size-1 인 것은 정확히 맞는
             * 줄과 잘린 줄이 구별되지 않는다. 셸에서는 그 차이가
             * 크다 - 긴 URL 이나 긴 JSON 한 줄이 조용히 반토막 나서
             * 엉뚱한 명령이 되고, 아무 데도 그 말이 없다.
             *
             * 그래서 여기서 한 번 말한다. 한 줄에 한 번만 말하고,
             * 나머지는 계속 읽어 버려서 줄 경계는 지킨다. */
            if (!warned) {
                warned = true;
                dprintf(STDERR_FILENO,
                        "readline: a line longer than %lu bytes was cut\n",
                        (unsigned long)(size - 1));
            }
        }
    }

    buf[n] = '\0';
    return (long)n;
}

/* ── Paged output ─────────────────────────────────────────────────────
 * See stdio.h. The state is a handful of file-scope variables because
 * only one thing can be paging at a time - this is a terminal, and there
 * is exactly one screen and one person reading it. */
static int  page_rows  = 0;    /* 0 means "not paging, print straight" */
static int  page_shown = 0;    /* lines printed on this screenful */
static int  page_tty   = -1;
static bool page_own_tty = false;

void page_begin(void)
{
    page_rows  = 0;
    page_shown = 0;
    page_tty   = -1;
    page_own_tty = false;

    /* Paging is for a person at a screen. Anything else - a pipe, a
     * file, `ssh board sysinfo` with no terminal on the far end - gets
     * the whole output and no prompt.
     *
     * This asks twice, on purpose. lp_term_size failing was already the
     * guard, and it is a good one: TIOCGWINSZ returns ENOTTY on a pipe.
     * But that is a guarantee that rests on an ioctl failing, and a
     * paged command that stops halfway through a pipe is exactly the
     * bug that makes a headless machine unscriptable - nothing comes
     * back, and the reason is a prompt nobody can see. So the intent is
     * written down as well: if stdout is not a terminal, do not page.
     */
    if (!lp_isatty(STDOUT_FILENO))
        return;

    int rows = 0, cols = 0;
    if (lp_term_size(STDOUT_FILENO, &rows, &cols) < 0)
        return;                      /* redirected: nothing to page */
    if (rows < 4)
        return;                      /* too small to be worth stopping */

    /* The keys have to come from the terminal, not from stdin - stdin may
     * be the pipe carrying the text we are paging. */
    long fd = lp_open("/dev/tty", O_RDONLY, 0);
    if (fd >= 0) {
        page_tty = (int)fd;
        page_own_tty = true;
    } else {
        page_tty = STDIN_FILENO;
    }

    page_rows = rows;
}

/* Ask, and wait. Returns false if the answer was "stop". */
static bool page_wait(void)
{
    fputs("-- more -- (space, enter, q) ", STDOUT_FILENO);

    lp_termios_t saved;
    bool raw = (lp_term_raw(page_tty, &saved) == 0);

    char c = ' ';
    long n = lp_read(page_tty, &c, 1);

    if (raw)
        lp_term_restore(page_tty, &saved);

    /* Wipe the prompt so it does not stay in the output. */
    fputs("\r                              \r", STDOUT_FILENO);

    if (n <= 0)
        return false;                /* the terminal went away */
    if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */)
        return false;

    if (c == '\r' || c == '\n')
        page_shown = page_rows - 2;  /* one more line, then ask again */
    else
        page_shown = 0;              /* a whole screen */
    return true;
}

bool page_line(const char *text)
{
    fputs(text, STDOUT_FILENO);
    fputs("\n", STDOUT_FILENO);

    if (page_rows == 0)
        return true;

    /* One line is kept for the prompt itself. */
    if (++page_shown >= page_rows - 1)
        return page_wait();

    return true;
}

void page_end(void)
{
    if (page_own_tty && page_tty >= 0)
        lp_close(page_tty);
    page_rows  = 0;
    page_tty   = -1;
    page_own_tty = false;
}
