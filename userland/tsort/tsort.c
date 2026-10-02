/* tsort - put a list of "this before that" pairs into a working order.
 *
 *   tsort deps            each line is "A B", meaning A must come before B
 *
 * The input is a partial order and the output is a total one consistent
 * with it. That is Knuth's Algorithm T: keep a count of how many things
 * still have to come before each item, print everything whose count is
 * zero, subtract one from each of its successors, repeat.
 *
 * Two details decide the exact output, and both are here on purpose
 * because a different choice gives a different - still correct - order,
 * and then this command and Ubuntu's disagree on the same file:
 *
 *   - items with a count of zero are collected by walking the tree of
 *     names in sorted order, so ties break alphabetically;
 *   - the successors of an item are held newest first, so the queue
 *     grows in the reverse of the order the pairs were read.
 *
 * When something is left over that never reaches zero, the input has a
 * cycle. tsort does not give up: it names the cycle on stderr, cuts one
 * edge out of it, and carries on, so you still get an order and an exit
 * status of 1 telling you not to trust it completely.
 *
 * A pair with the same name twice ("a a") is not a cycle. It is what a
 * makefile writes when a rule depends on nothing, and tsort drops it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PROG "tsort"

typedef struct item item_t;

typedef struct succ {
    item_t      *suc;
    struct succ *next;
} succ_t;

struct item {
    char    *str;
    item_t  *left, *right;
    int      height;
    bool     printed;
    size_t   count;       /* how many things must still come before this */
    item_t  *qlink;       /* next in the output queue, and in a found cycle */
    succ_t  *top;         /* what must come after this, newest first */
};

static item_t *head;      /* front of the queue of ready items */
static item_t *zeros;     /* back of it */
static item_t *loop;      /* the cycle being traced, while tracing one */
static size_t  n_strings;
static bool    ok = true;

static void oom(void)
{
    dprintf(STDERR_FILENO, PROG ": memory exhausted\n");
    lp_exit(1);
}

/* ── the set of names ──
 *
 * A balanced tree, and the balancing is not for speed alone: reading the
 * output of `sort` would turn a plain search tree into a linked list,
 * and both the insert and the walk are recursive. Rotating cannot change
 * the answer, because an in-order walk of a search tree is in sorted
 * order whatever its shape. */
static int height(item_t *n) { return n ? n->height : 0; }

static void refit(item_t *n)
{
    int l = height(n->left), r = height(n->right);
    n->height = (l > r ? l : r) + 1;
}

static item_t *rot_right(item_t *y)
{
    item_t *x = y->left;
    y->left = x->right; x->right = y;
    refit(y); refit(x);
    return x;
}

static item_t *rot_left(item_t *x)
{
    item_t *y = x->right;
    x->right = y->left; y->left = x;
    refit(x); refit(y);
    return y;
}

static item_t *rebalance(item_t *n)
{
    refit(n);
    int b = height(n->left) - height(n->right);
    if (b > 1) {
        if (height(n->left->left) < height(n->left->right))
            n->left = rot_left(n->left);
        return rot_right(n);
    }
    if (b < -1) {
        if (height(n->right->right) < height(n->right->left))
            n->right = rot_right(n->right);
        return rot_left(n);
    }
    return n;
}

static item_t *new_item(const char *s)
{
    item_t *k = calloc(1, sizeof *k);
    char   *d = strdup(s);
    if (!k || !d) oom();
    k->str = d;
    k->height = 1;
    return k;
}

static item_t *insert(item_t *n, const char *s, item_t **found)
{
    if (!n) return (*found = new_item(s));
    int c = strcmp(s, n->str);
    if (c == 0) { *found = n; return n; }
    if (c < 0) n->left  = insert(n->left,  s, found);
    else       n->right = insert(n->right, s, found);
    return rebalance(n);
}

/* Stops early when the action says it is finished, which is how the
 * cycle tracer gets to abandon the walk the moment it has the answer. */
static bool walk(item_t *n, bool (*act)(item_t *))
{
    if (!n) return false;
    if (walk(n->left, act)) return true;
    if (act(n)) return true;
    return walk(n->right, act);
}

static bool count_items(item_t *k) { (void)k; n_strings++; return false; }

static bool scan_zeros(item_t *k)
{
    if (k->count == 0 && !k->printed) {
        if (head == NULL) head = k;
        else              zeros->qlink = k;
        zeros = k;
    }
    return false;
}

/* ── finding a cycle ──
 *
 * Everything still left has a predecessor, so walking backwards from any
 * of them has to come back to something already seen. The trail is kept
 * in qlink, newest first; when a step lands on a node already on the
 * trail, the piece of the trail from there is the cycle. It takes more
 * than one pass over the tree, because the tree is in name order and has
 * nothing to do with which node the trail wants next. */
static bool detect_loop(item_t *k)
{
    if (k->count == 0)
        return false;

    if (loop == NULL) {          /* start a trail here */
        loop = k;
        return false;
    }

    for (succ_t **p = &k->top; *p; p = &(*p)->next) {
        if ((*p)->suc != loop)
            continue;

        if (!k->qlink) {         /* k is new to the trail: step back to it */
            k->qlink = loop;
            loop = k;
            return false;
        }

        /* k is already on the trail, so the trail closed. Name it from
         * the newest end round to k, then cut the edge k -> loop so the
         * sort can make progress. */
        while (loop) {
            item_t *next = loop->qlink;
            dprintf(STDERR_FILENO, PROG ": %s\n", loop->str);
            if (loop == k) {
                succ_t *s = *p;
                s->suc->count--;
                *p = s->next;
                free(s);
                break;
            }
            loop->qlink = NULL;
            loop = next;
        }
        while (loop) {           /* clear whatever trail is left */
            item_t *next = loop->qlink;
            loop->qlink = NULL;
            loop = next;
        }
        return true;
    }
    return false;
}

/* ── reading tokens ──
 * Names are separated by spaces, tabs and newlines and by nothing else;
 * a carriage return is part of the name, as it is in GNU. */
static char   rbuf[65536];
static size_t rlen, rpos;
static int    rfd;
static bool   read_failed;
static int    read_err;

static int rgetc(void)
{
    if (rpos < rlen)
        return (unsigned char)rbuf[rpos++];
    long n = lp_read(rfd, rbuf, sizeof rbuf);
    if (n <= 0) {
        if (n < 0) { read_failed = true; read_err = (int)-n; }
        return -1;
    }
    rlen = (size_t)n; rpos = 0;
    return (unsigned char)rbuf[rpos++];
}

static bool is_delim(int c) { return c == ' ' || c == '\t' || c == '\n'; }

static char  *tok;
static size_t tokcap;

static bool next_token(void)
{
    int c;
    do { c = rgetc(); } while (is_delim(c));
    if (c < 0) return false;

    size_t len = 0;
    do {
        if (len + 1 >= tokcap) {
            size_t cap = tokcap ? tokcap * 2 : 128;
            char *nb = realloc(tok, cap);
            if (!nb) oom();
            tok = nb; tokcap = cap;
        }
        tok[len++] = (char)c;
        c = rgetc();
    } while (c >= 0 && !is_delim(c));
    tok[len] = '\0';
    return true;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'H' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        if (c == 'H') {
            printf("Usage: tsort [OPTION] [FILE]\n"
                   "Write totally ordered list consistent with the partial"
                   " ordering in FILE.\n\n"
                   "With no FILE, or when FILE is -, read standard input.\n\n"
                   "      --help        display this help and exit\n");
            return 0;
        }
        lp_getopt_err(PROG, &g);
        return 1;
    }

    if (argc - g.ind > 1) {
        dprintf(STDERR_FILENO, PROG ": extra operand '%s'\n", argv[g.ind + 1]);
        dprintf(STDERR_FILENO, "Try '" PROG " --help' for more information.\n");
        return 1;
    }

    const char *file = (g.ind == argc) ? "-" : argv[g.ind];
    if (strcmp(file, "-") == 0) {
        rfd = STDIN_FILENO;
    } else {
        long fd = lp_open(file, O_RDONLY, 0);
        if (fd < 0) {
            lp_diag(PROG, NULL, NULL, "cannot open", file, (int)-fd);
            return 1;
        }
        rfd = (int)fd;
    }

    item_t *root = NULL, *j = NULL, *k = NULL;

    while (next_token()) {
        item_t *cur;
        root = insert(root, tok, &cur);
        k = cur;
        if (j) {
            /* Record that j comes before k. A pair of the same name says
             * nothing and would look like a cycle if it were kept. */
            if (strcmp(j->str, k->str) != 0) {
                succ_t *s = malloc(sizeof *s);
                if (!s) oom();
                k->count++;
                s->suc = k;
                s->next = j->top;
                j->top = s;
            }
            k = NULL;
        }
        j = k;
    }
    if (read_failed) {
        dprintf(STDERR_FILENO, PROG ": %s: read error: %s\n",
                file, lp_strerror(read_err));
        return 1;
    }
    if (k) {
        dprintf(STDERR_FILENO,
                PROG ": %s: input contains an odd number of tokens\n", file);
        return 1;
    }

    walk(root, count_items);

    while (n_strings > 0) {
        walk(root, scan_zeros);

        while (head) {
            printf("%s\n", head->str);
            head->printed = true;
            n_strings--;

            for (succ_t *p = head->top; p; p = p->next)
                if (--p->suc->count == 0) {
                    zeros->qlink = p->suc;
                    zeros = p->suc;
                }

            head = head->qlink;
        }

        if (n_strings > 0) {
            dprintf(STDERR_FILENO, PROG ": %s: input contains a loop:\n", file);
            ok = false;
            do
                walk(root, detect_loop);
            while (loop);
        }
    }

    return ok ? 0 : 1;
}
