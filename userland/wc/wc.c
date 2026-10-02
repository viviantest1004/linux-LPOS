/* wc - count lines, words, bytes and characters.
 *
 *   wc [-l] [-w] [-c] [-m] [-L] [file]...
 *
 * With no flags it prints lines, words and bytes, in that order. With no
 * file it reads what is piped in, which is the common use:
 * "help | wc -l".
 *
 * ── -c is bytes and -m is characters ──
 * This used to count characters and call it -c, on the reasoning that a
 * Hangul syllable is one character and three bytes, and that a system
 * which is UTF-8 throughout should say one. The reasoning was fine and
 * the letter was wrong: every other wc in the world answers -c in
 * bytes, and -m is the one that answers in characters.
 *
 * It matters more than a letter usually does, because nothing about the
 * output says which it is. `wc -c` on a 586924-byte program printed
 * 494305 - a plausible number, off by exactly the multi-byte sequences
 * in it, and no way to tell from looking. A script checking a download
 * against its published size would have failed and blamed the download.
 *
 * So: -c bytes, -m characters, and both are counted in the same pass so
 * asking for both costs nothing.
 *
 * ── Where this still differs from GNU ──
 * On text - ASCII, Hangul, a file with one corrupt byte in it, a file
 * that ends mid-character, overlong forms, surrogates - every count
 * here matches GNU's exactly. On a file of uniformly random bytes, -m,
 * -w and -L can differ by a fraction of a percent, because at that
 * point the answer is decided by how mbrtowc's state machine resynchs
 * after each invalid sequence rather than by anything about the file.
 * -c and -l match there too, and those are the two anybody asks of a
 * file that is not text.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* s64, not long. On a 32-bit machine a long stops at two billion, and
 * `wc -c` on a file bigger than that reported the remainder rather than
 * failing - the one kind of wrong answer that looks like a real one. */
typedef struct { s64 lines, words, bytes, chars, maxlen; } count_t;

/* One pass, counting everything.
 *
 * The columns a caller asked for are chosen when printing, not here:
 * the work is dominated by reading the file, so counting the other
 * three costs nothing measurable and keeps this loop with one shape
 * instead of five.
 */
/* The code point a completed UTF-8 sequence stands for. An invalid
 * sequence comes back as something meaningless, which is fine: nothing
 * below asks anything of it except whether it is a space, and it is
 * not. */
static u32 utf8_cp(const char *s, int len)
{
    unsigned char c0 = (unsigned char)s[0];
    if (len <= 1) return c0;
    u32 cp = (u32)(c0 & (0xFF >> (len + 1)));
    for (int i = 1; i < len; i++)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}

/* Whether a well-formed-looking sequence is actually a character.
 *
 * The shape of the bytes is not enough. The same code point can be
 * written in more than one length, and only the shortest is a
 * character - "overlong" forms were how a filename could be made to
 * look like one thing to a check and another to the filesystem. The
 * surrogate range belongs to UTF-16 and is not a character here, and
 * nothing exists above U+10FFFF. On random data these are common
 * enough that ignoring them made -m and -L disagree with every other
 * wc by thousands. */
static bool utf8_valid(const char *bytes, int len)
{
    u32 cp = utf8_cp(bytes, len);
    if (len == 2) return cp >= 0x80;
    if (len == 3) return cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
    if (len == 4) return cp >= 0x10000 && cp <= 0x10FFFF;
    return true;
}

/* What separates one word from the next.
 *
 * Not just the ASCII five. A Korean or Japanese document written with
 * the ideographic space U+3000 - which is the space character on those
 * keyboards - would otherwise count as one enormous word, and a page
 * pasted from a browser is full of U+00A0. Every other wc in a UTF-8
 * locale splits on these, so this one does too. */
static bool uni_space(u32 cp)
{
    if (cp == 0x20 || (cp >= 0x09 && cp <= 0x0D)) return true;
    if (cp >= 0x2000 && cp <= 0x200A) return true;
    switch (cp) {
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000:
        return true;
    }
    return false;
}

/* One character, once it is known to be whole.
 *
 * Everything that has to look at a character rather than a byte goes
 * through here: the line count, the word count and the display width.
 */
typedef struct { count_t *c; s64 width; bool in_word; } wcst_t;

static void emit(wcst_t *st, const char *bytes, int len)
{
    u32 cp = utf8_cp(bytes, len);

    st->c->chars++;

    if (cp == '\t')
        st->width += 8 - (st->width % 8);       /* to the next tab stop */
    else if (cp == '\n' || cp == '\r')
        ;                                       /* no width of their own */
    else
        st->width += (s64)utf8_str_width(bytes, (size_t)len);

    if (cp == '\n') {
        st->c->lines++;
        if (st->width > st->c->maxlen) st->c->maxlen = st->width;
        st->width = 0;
    }

    if (uni_space(cp))
        st->in_word = false;
    else if (!st->in_word) {
        st->in_word = true;
        st->c->words++;
    }
}

/* One pass, counting everything.
 *
 * The columns a caller asked for are chosen when printing, not here:
 * the work is dominated by reading the file, so counting the other
 * four costs nothing measurable and keeps this loop with one shape
 * instead of five.
 *
 * The decoder gives up on a sequence rather than trusting its length
 * byte. A lead byte that says "three bytes follow" and is then followed
 * by something that is not a continuation is not a three-byte
 * character: it is one broken byte and then whatever came next. Reading
 * it the trusting way swallowed the bytes after it, and a text file
 * with one corrupt byte in it came back short a line - which is the
 * sort of thing wc is used to check for in the first place.
 */
static void count_fd(int fd, count_t *c)
{
    char buf[4096];
    wcst_t st = { c, 0, false };
    char pend[4];             /* the sequence being assembled */
    int  plen = 0, want = 0;

    for (;;) {
        long n = lp_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        c->bytes += n;

        for (long i = 0; i < n; ) {
            unsigned char ch = (unsigned char)buf[i];

            if (plen == 0) {
                if (ch < 0x80) {                /* plain ASCII */
                    char one = (char)ch;
                    emit(&st, &one, 1);
                    i++;
                    continue;
                }
                want = utf8_seq_len(ch);
                if (want < 2 || want > 4) {     /* a stray continuation, or 0xF8+ */
                    i++;
                    continue;                   /* counts as a byte and nothing else */
                }
                pend[plen++] = (char)ch;
                i++;
                continue;
            }

            if ((ch & 0xC0) == 0x80) {          /* a continuation, as promised */
                pend[plen++] = (char)ch;
                i++;
                if (plen == want) {
                    if (utf8_valid(pend, plen))
                        emit(&st, pend, plen);
                    plen = 0;                   /* else: bytes, and nothing more */
                }
                continue;
            }

            /* Not a continuation. What we were holding was never a
             * character, so it is dropped: it still counts in -c, which
             * is a count of bytes, and in nothing else. That is what
             * every other wc does with a byte that decodes to no
             * character, and it is the only reading that leaves -m and
             * -L meaning what they say. This byte is not consumed - it
             * starts again from the top. */
            plen = 0;
        }
    }

    /* A sequence that ran out of file is not a character either. */

    /* A last line with no newline on the end still has a length. */
    if (st.width > c->maxlen) c->maxlen = st.width;
}

/* How wide the number columns are.
 *
 * A fixed %7ld looked tidy and was wrong: `wc -l < f` printed
 * "      3" where every other wc prints "3", so a script doing
 * `n=$(wc -l < f)` got a string with six spaces in front of it. GNU
 * sizes the column to the largest number it is about to print - across
 * every file, so the lines still line up - and that means the counts
 * have to be collected before any of them is printed. */
static int digits(s64 v)
{
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

/* Which columns were asked for. GNU prints them in this order whatever
 * order the flags were given in. */
static bool o_lines, o_words, o_chars, o_bytes, o_maxlen;

static void report(const count_t *c, const char *name, int width)
{
    bool first = true;
    s64  cols[5];
    int  n = 0;
    if (o_lines)  cols[n++] = c->lines;
    if (o_words)  cols[n++] = c->words;
    if (o_chars)  cols[n++] = c->chars;
    if (o_bytes)  cols[n++] = c->bytes;
    if (o_maxlen) cols[n++] = c->maxlen;

    for (int i = 0; i < n; i++) {
        printf("%*lld", first ? width : width + 1, (long long)cols[i]);
        first = false;
    }
    if (name) printf(" %s", name);
    printf("\n");
}

static s64 widest(const count_t *c)
{
    s64 m = 0;
    if (o_lines  && c->lines  > m) m = c->lines;
    if (o_words  && c->words  > m) m = c->words;
    if (o_chars  && c->chars  > m) m = c->chars;
    if (o_bytes  && c->bytes  > m) m = c->bytes;
    if (o_maxlen && c->maxlen > m) m = c->maxlen;
    return m;
}

static void usage(int fd)
{
    dprintf(fd,
        "Usage: wc [OPTION]... [FILE]...\n"
        "Print newline, word, and byte counts for each FILE, and a total\n"
        "line if more than one FILE is given.  With no FILE, read standard\n"
        "input.\n\n"
        "  -c, --bytes            print the byte counts\n"
        "  -m, --chars            print the character counts\n"
        "  -l, --lines            print the newline counts\n"
        "  -L, --max-line-length  print the maximum display width\n"
        "  -w, --words            print the word counts\n"
        "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 0, 'c' }, { "chars", 0, 'm' }, { "lines", 0, 'l' },
        { "words", 0, 'w' }, { "max-line-length", 0, 'L' },
        { "help", 0, 'H' }, { NULL, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "clmwL", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'c': o_bytes  = true; break;
        case 'm': o_chars  = true; break;
        case 'l': o_lines  = true; break;
        case 'w': o_words  = true; break;
        case 'L': o_maxlen = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:
            lp_getopt_err("wc", &g);
            usage(STDERR_FILENO);
            return 1;
        }
    }

    if (!o_lines && !o_words && !o_chars && !o_bytes && !o_maxlen)
        o_lines = o_words = o_bytes = true;

    if (g.ind >= argc) {
        count_t c = { 0, 0, 0, 0, 0 };
        count_fd(STDIN_FILENO, &c);
        report(&c, NULL, digits(widest(&c)));
        return 0;
    }

    /* Collected first, printed after, so the width is the same on every
     * line - which is the point of having one.
     *
     * The failures are collected too, rather than printed as they
     * happen. The counts cannot be printed until the widest of them is
     * known, so an error printed at the moment it happened landed above
     * every count - and `wc a nosuch b` read as though all three had
     * failed. Held in operand order with the counts, the output says
     * which one it was. */
    typedef struct { const char *name; bool ok; int err; count_t c; } item_t;
    static item_t items[256];
    count_t total = { 0, 0, 0, 0, 0 };
    int rc = 0, seen = 0, counted = 0;

    for (int i = g.ind; i < argc && seen < 256; i++) {
        count_t c = { 0, 0, 0, 0, 0 };
        items[seen].name = argv[i];
        items[seen].ok   = true;
        items[seen].err  = 0;

        if (strcmp(argv[i], "-") == 0) {
            count_fd(STDIN_FILENO, &c);
        } else {
            long fd = lp_open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                items[seen].ok  = false;
                items[seen].err = (int)-fd;
                seen++;
                rc = 1;
                continue;
            }
            count_fd((int)fd, &c);
            lp_close((int)fd);
        }

        items[seen].c = c;
        seen++;
        counted++;
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        total.chars += c.chars;
        if (c.maxlen > total.maxlen) total.maxlen = c.maxlen;
    }

    s64 m = 0;
    for (int i = 0; i < seen; i++) {
        if (!items[i].ok) continue;
        s64 v = widest(&items[i].c);
        if (v > m) m = v;
    }
    /* The total line appears whenever more than one file was named, not
     * whenever more than one could be read - which is what GNU does, and
     * what makes `wc *.c` still add up when one of them is unreadable. */
    bool want_total = (argc - g.ind) > 1;
    if (want_total) {
        s64 v = widest(&total);
        if (v > m) m = v;
    }
    int width = digits(m);

    for (int i = 0; i < seen; i++) {
        if (items[i].ok) {
            report(&items[i].c, items[i].name, width);
        } else {
            lp_diag("wc", NULL, NULL, "cannot open", items[i].name,
                    items[i].err);
        }
    }
    if (want_total)
        report(&total, "total", width);
    (void)counted;
    return rc;
}
