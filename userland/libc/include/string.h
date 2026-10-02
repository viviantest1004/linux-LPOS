/* string.h - strings and memory, our own implementations. */
#ifndef _LP_STRING_H
#define _LP_STRING_H

#include "types.h"

void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strdup(const char *s);

/* A safe copy that reports truncation. The destination is always NUL
 * terminated. It returns the length it wanted to copy, so >= size means
 * it was truncated. */
size_t strlcpy(char *d, const char *s, size_t size);
/* Append s onto d, always NUL terminating.
 * Returns the full length it wanted; >= size means it was truncated. */
size_t strlcat(char *d, const char *s, size_t size);

/* ── UTF-8 ──
 *
 * Drawing text on a terminal means keeping three different lengths apart.
 *
 *   bytes     One Hangul character is 3 of them. Move the cursor by a
 *             single byte and it lands mid-character; erase there and
 *             the character breaks.
 *   characters Not usable as an array index.
 *   columns   Hangul and Han take 2. Count them as 1 and the cursor is
 *
 *             drawn left of where it really is, and line clipping is off.
 *
 * Mix the three up and every line containing Hangul goes wrong. */

/* Bytes in the character this lead byte starts (1-4).
 * 1 for an invalid byte, so a corrupt file still makes progress. */
int    utf8_seq_len(unsigned char lead);

/* Read one code point from s[0..max) and put the bytes used in *used.
 * An invalid sequence gives 0xFFFD with *used = 1. */
u32    utf8_decode(const char *s, size_t max, int *used);

/* Screen columns: 0 for combining marks, 2 for Hangul and Han, 1 otherwise. */
int    utf8_width(u32 cp);

/* Step a character back or forward. idx is a byte offset. Stays in bounds. */
size_t utf8_prev(const char *s, size_t idx);
size_t utf8_next(const char *s, size_t len, size_t idx);

/* The screen width of s[0..len) once drawn. */
size_t utf8_str_width(const char *s, size_t len);

long      strtol(const char *s, char **end, int base);
/* The same parse, full width. On a 32-bit machine a long stops at two
 * billion; anything that means a file size, an offset or a date needs
 * this one. */
long long strtoll(const char *s, char **end, int base);

#endif /* _LP_STRING_H */
