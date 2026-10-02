/* stdlib.h - memory allocation and the environment. */
#ifndef _LP_STDLIB_H
#define _LP_STDLIB_H

#include "types.h"

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

char *getenv(const char *name);
/* Set a variable so that children inherit it. overwrite=0 keeps an
 * existing value. Returns 0, or -1 when there is no memory. */
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   atoi(const char *s);

#endif /* _LP_STDLIB_H */
