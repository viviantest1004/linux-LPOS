/* malloc.c - a minimal brk-based allocator.
 *
 * There are no threads here - only the shell and init run - and the
 * allocation pattern is simple, so one free list is enough. It coalesces
 * adjacent blocks and stops there; more is not worth the complexity. */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

typedef struct block {
    size_t        size;     /* usable bytes, not counting the header */
    struct block *next;     /* the whole list, in address order */
    int           free;
} block_t;

#define HDR_SIZE   ((size_t)sizeof(block_t))
#define ALIGN_UP(n) (((n) + 15UL) & ~15UL)   /* 16-byte alignment */
#define GROW_MIN    (64UL * 1024UL)          /* smallest step when growing brk */

static block_t *heap_head = NULL;
static char    *brk_cur   = NULL;

/* Get or extend the current break. NULL on failure. */
static void *heap_grow(size_t need)
{
    if (brk_cur == NULL) {
        long cur = sys_call1(SYS_brk, 0);
        if (cur <= 0)
            return NULL;
        brk_cur = (char *)cur;
    }

    size_t grow = need < GROW_MIN ? GROW_MIN : ALIGN_UP(need);
    char  *want = brk_cur + grow;

    long got = sys_call1(SYS_brk, (long)want);
    if ((char *)got < want)
        return NULL;            /* the kernel refused */

    void *old = brk_cur;
    brk_cur = (char *)got;
    return old;
}

void *malloc(size_t n)
{
    if (n == 0)
        return NULL;
    n = ALIGN_UP(n);

    /* 1) first fit from the free list */
    block_t *prev = NULL;
    for (block_t *b = heap_head; b; prev = b, b = b->next) {
        if (!b->free || b->size < n)
            continue;

        /* Split it if the leftover exceeds a header plus a minimum block. */
        if (b->size >= n + HDR_SIZE + 16) {
            block_t *split = (block_t *)((char *)b + HDR_SIZE + n);
            split->size = b->size - n - HDR_SIZE;
            split->free = 1;
            split->next = b->next;
            b->size = n;
            b->next = split;
        }
        b->free = 0;
        return (char *)b + HDR_SIZE;
    }

    /* 2) grow the heap */
    void *mem = heap_grow(n + HDR_SIZE);
    if (!mem)
        return NULL;

    block_t *b = mem;
    b->size = n;
    b->free = 0;
    b->next = NULL;

    if (prev) prev->next = b;
    else      heap_head  = b;

    /* If heap_grow took more than we asked, the rest becomes a free block. */
    size_t used = HDR_SIZE + n;
    size_t have = (size_t)(brk_cur - (char *)mem);
    if (have >= used + HDR_SIZE + 16) {
        block_t *rest = (block_t *)((char *)mem + used);
        rest->size = have - used - HDR_SIZE;
        rest->free = 1;
        rest->next = NULL;
        b->next = rest;
    }
    return (char *)b + HDR_SIZE;
}

void free(void *p)
{
    if (!p)
        return;

    block_t *b = (block_t *)((char *)p - HDR_SIZE);
    b->free = 1;

    /* Merge the free blocks that follow into one. The list is in address
     * order, so looking at next is enough. */
    while (b->next && b->next->free &&
           (char *)b->next == (char *)b + HDR_SIZE + b->size) {
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
    }
}

void *calloc(size_t count, size_t size)
{
    /* Check the multiplication for overflow. Without this, an overflow
     * hands back a small buffer that the caller writes to as if it were big. */
    if (count && size > (size_t)-1 / count)
        return NULL;

    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)  return malloc(n);
    if (!n)  { free(p); return NULL; }

    block_t *b = (block_t *)((char *)p - HDR_SIZE);
    if (b->size >= n)
        return p;

    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, b->size);
    free(p);
    return np;
}

extern char **environ;

char *getenv(const char *name)
{
    if (!environ)
        return NULL;

    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}

/* Set a variable, and make sure children inherit it.
 *
 * The environment we were handed by execve is an array we do not own and
 * cannot extend, so the first write copies it somewhere we can grow. From
 * then on the array is ours: environ points at it, and execve passes it
 * to every child. */
static char **env_owned  = NULL;
static int    env_count  = 0;
static int    env_capacity = 0;

int setenv(const char *name, const char *value, int overwrite)
{
    size_t nlen = strlen(name);

    /* Already there? Replace the whole "NAME=value" string. */
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            if (strncmp(environ[i], name, nlen) != 0 || environ[i][nlen] != '=')
                continue;
            if (!overwrite)
                return 0;

            char *entry = malloc(nlen + strlen(value) + 2);
            if (!entry)
                return -1;
            strcpy(entry, name);
            entry[nlen] = '=';
            strcpy(entry + nlen + 1, value);
            environ[i] = entry;
            return 0;
        }
    }

    /* Take a copy of the array we can extend. */
    if (!env_owned) {
        env_count = 0;
        if (environ)
            while (environ[env_count])
                env_count++;

        env_capacity = env_count + 16;
        env_owned = malloc(sizeof(char *) * (size_t)(env_capacity + 1));
        if (!env_owned)
            return -1;
        for (int i = 0; i < env_count; i++)
            env_owned[i] = environ[i];
        env_owned[env_count] = NULL;
        environ = env_owned;
    }

    if (env_count + 1 >= env_capacity) {
        int   grown = env_capacity * 2;
        char **bigger = malloc(sizeof(char *) * (size_t)(grown + 1));
        if (!bigger)
            return -1;
        for (int i = 0; i < env_count; i++)
            bigger[i] = env_owned[i];
        env_owned    = bigger;
        env_capacity = grown;
        environ      = env_owned;
    }

    char *entry = malloc(nlen + strlen(value) + 2);
    if (!entry)
        return -1;
    strcpy(entry, name);
    entry[nlen] = '=';
    strcpy(entry + nlen + 1, value);

    env_owned[env_count++] = entry;
    env_owned[env_count]   = NULL;
    return 0;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

/* Take a name back out of the environment.
 *
 * "빈 값으로 두면 된다" 고 오래 적어 두었는데, 맞지 않는 자리가 하나
 * 있다. 셸의 `local x` 는 x 를 **없는** 상태로 만들어야 하고,
 * 스크립트는 그것을 `[ -z "$x" ]` 가 아니라 `${x-기본값}` 이나
 * `[ -n "${x+set}" ]` 로 본다. 빈 값과 없음은 거기서 다른 답을 낸다.
 *
 * 배열에서 그 자리를 빼고 뒤를 한 칸씩 당긴다. 문자열 자체는 두는데,
 * execve 가 우리에게 준 원래 배열의 것일 수 있어서 free 할 수 없기
 * 때문이다 - 우리가 만든 것 몇 개가 남는 편이 남의 것을 free 하는
 * 것보다 낫다. */
int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '='))
        return -1;
    if (!environ)
        return 0;

    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) != 0 || environ[i][nlen] != '=')
            continue;

        int j = i;
        while (environ[j]) {
            environ[j] = environ[j + 1];
            j++;
        }
        if (env_owned == environ && env_count > 0)
            env_count--;
        i--;                        /* 같은 이름이 두 번 있을 수도 있다 */
    }
    return 0;
}
