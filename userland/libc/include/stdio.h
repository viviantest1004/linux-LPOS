/* stdio.h - minimal I/O with no FILE*. File descriptors are used directly. */
#ifndef _LP_STDIO_H
#define _LP_STDIO_H

#include "types.h"

int  printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int  dprintf(int fd, const char *fmt, ...)
                                   __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t size, const char *fmt, ...)
                                   __attribute__((format(printf, 3, 4)));

int  puts(const char *s);          /* adds a newline */
int  fputs(const char *s, int fd); /* no newline */
int  putchar(int c);

/* Read one line. The newline is dropped and the result NUL terminated.
 * Returns the length read, or -1 at EOF. */
long readline(int fd, char *buf, size_t size);
/* One record ending at `delim`, bytes untouched. -1 at end of input.
 * saw_delim (may be NULL) tells a record that ended at a delimiter from
 * one that ended at end of input. */
long readrec(int fd, char *buf, size_t size, char delim, bool *saw_delim);

/* ── Paged output ─────────────────────────────────────────────────────
 * A framebuffer console keeps no scrollback. Anything that scrolls off
 * the top is not "further up" - it is gone, and there is no key that
 * brings it back. So output longer than the screen has to stop at the
 * bottom and wait, or it may as well not have been printed.
 *
 * This only happens when the output really is going to a screen. Piped
 * or redirected, every line goes straight through and nothing waits for
 * a key that will never come.
 *
 *   page_begin();
 *   while (...) if (!page_line(text)) break;   // false: the user quit
 *   page_end();
 *
 * Keys come from the terminal itself rather than stdin, so `foo | more`
 * still reads them while stdin carries the data being paged. */
void page_begin(void);
bool page_line(const char *text);
void page_end(void);

#endif /* _LP_STDIO_H */
