/* string.c - the string and memory implementations. */
#include "string.h"
#include "stdlib.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    u8  v = (u8)c;

    while (n && ((uintptr_t)d & 7)) { *d++ = v; n--; }

    u64 w = 0x0101010101010101UL * v;
    while (n >= 8) { *(u64 *)d = w; d += 8; n -= 8; }

    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        while (n >= 8) { *(u64 *)d = *(const u64 *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);

    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = '\0';
    return r;
}

size_t strlcpy(char *d, const char *s, size_t size)
{
    size_t slen = strlen(s);

    if (size) {
        size_t copy = (slen >= size) ? size - 1 : slen;
        memcpy(d, s, copy);
        d[copy] = '\0';
    }
    return slen;    /* >= size means it was truncated */
}

size_t strlcat(char *d, const char *s, size_t size)
{
    /* Find the NUL in d. With no NUL inside size, d is not a string, so
     * do nothing and just report the length we wanted. */
    size_t dlen = 0;
    while (dlen < size && d[dlen] != '\0')
        dlen++;

    size_t slen = strlen(s);
    if (dlen == size)
        return size + slen;

    size_t room = size - dlen;      /* includes room for the NUL */
    size_t copy = (slen >= room) ? room - 1 : slen;
    memcpy(d + dlen, s, copy);
    d[dlen + copy] = '\0';
    return dlen + slen;             /* >= size means it was truncated */
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;

    size_t nlen = strlen(needle);
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, nlen) == 0)
            return (char *)hay;
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* strtoll is the real one; strtol is it narrowed to a long.
 *
 * They were one function returning `long` until this OS was built for
 * a 32-bit machine, where a long is four bytes. Everything that parses
 * a number - expr, seek offsets, timestamps - went through it, so on
 * that machine a file offset or an arithmetic result silently stopped
 * at two billion. The parse itself is the same either way; only the
 * width of the answer differs, so there is one body and a wrapper. */
long long strtoll(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    long long val = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;

        if (d >= base) break;
        val = val * base + d;
        s++;
    }

    if (end) *end = (char *)s;
    return neg ? -val : val;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoll(s, end, base);
}


/* ── UTF-8 ─────────────────────────────────────────────────────── */

int utf8_seq_len(unsigned char lead)
{
    if (lead < 0x80)           return 1;   /* ASCII */
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;   /* Hangul lives here */
    if ((lead & 0xF8) == 0xF0) return 4;   /* emoji and such */
    return 1;   /* a continuation byte (10xxxxxx) or junk: step one at a time */
}

u32 utf8_decode(const char *s, size_t max, int *used)
{
    if (max == 0) { *used = 0; return 0; }

    unsigned char b0 = (unsigned char)s[0];
    int n = utf8_seq_len(b0);

    if (n == 1 || (size_t)n > max) {
        *used = 1;
        return (b0 < 0x80) ? (u32)b0 : 0xFFFDu;
    }

    /* Continuation bytes must be 10xxxxxx, or the sequence is broken. */
    for (int i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            *used = 1;
            return 0xFFFDu;
        }
    }

    u32 cp;
    switch (n) {
    case 2:  cp = (u32)(b0 & 0x1F); break;
    case 3:  cp = (u32)(b0 & 0x0F); break;
    default: cp = (u32)(b0 & 0x07); break;
    }
    for (int i = 1; i < n; i++)
        cp = (cp << 6) | (u32)((unsigned char)s[i] & 0x3F);

    *used = n;
    return cp;
}

int utf8_width(u32 cp)
{
    /* Combining marks sit on top of the previous character and take no
     * space of their own. Conjoining Hangul jamo (1160-11FF) count here too. */
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1160 && cp <= 0x11FF) ||
        (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F))
        return 0;

    if (cp < 0x1100)
        return 1;                          /* ASCII and Latin */

    /* East Asian Wide (W) and Fullwidth (F). Only the ranges from
     * EastAsianWidth that actually come up. */
    if ((cp >= 0x1100  && cp <= 0x115F)  ||  /* Hangul jamo */
        (cp >= 0x2E80  && cp <= 0x303E)  ||  /* radicals, CJK symbols */
        (cp >= 0x3041  && cp <= 0x33FF)  ||  /* kana, compatibility Hangul */
        (cp >= 0x3400  && cp <= 0x4DBF)  ||  /* Han extension A */
        (cp >= 0x4E00  && cp <= 0x9FFF)  ||  /* Han */
        (cp >= 0xA000  && cp <= 0xA4CF)  ||
        (cp >= 0xA960  && cp <= 0xA97F)  ||
        (cp >= 0xAC00  && cp <= 0xD7A3)  ||  /* Hangul syllables */
        (cp >= 0xF900  && cp <= 0xFAFF)  ||
        (cp >= 0xFE10  && cp <= 0xFE19)  ||
        (cp >= 0xFE30  && cp <= 0xFE6F)  ||
        (cp >= 0xFF00  && cp <= 0xFF60)  ||  /* fullwidth ASCII */
        (cp >= 0xFFE0  && cp <= 0xFFE6)  ||
        (cp >= 0x1F300 && cp <= 0x1F64F) ||  /* emoji */
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;

    return 1;
}

size_t utf8_prev(const char *s, size_t idx)
{
    if (idx == 0)
        return 0;
    /* Step back over continuation bytes to the lead byte. At most three,
     * so a corrupt file cannot walk us back to the start of the line. */
    size_t i    = idx - 1;
    int    back = 0;
    while (i > 0 && back < 3 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
        back++;
    }
    return i;
}

size_t utf8_next(const char *s, size_t len, size_t idx)
{
    if (idx >= len)
        return len;
    size_t n = (size_t)utf8_seq_len((unsigned char)s[idx]);
    return (idx + n > len) ? len : idx + n;
}

size_t utf8_str_width(const char *s, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; i < len; ) {
        int used;
        u32 cp = utf8_decode(s + i, len - i, &used);
        if (used == 0)
            break;
        w += (size_t)utf8_width(cp);
        i += (size_t)used;
    }
    return w;
}
