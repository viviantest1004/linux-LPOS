/* getopt - the option grammar every GNU tool shares.
 *
 * Each command here used to parse its own flags with a chain of strcmp,
 * and every one of them got a slightly different subset right. `head -n1`
 * printed a filename banner, `tail -n2` looked for a file called "-n2",
 * `id -u` complained about a user named "-u". None of that is exotic
 * syntax - it is what a person types every day - and the four rules that
 * make it work are the same for all of them:
 *
 *     -n5        a value attached to its letter
 *     -n 5       a value in the next word
 *     -abc       three letters bundled behind one dash
 *     --lines=5  the long spelling, with or without =
 *
 * plus two more that are easy to forget: "--" ends the options, and a
 * lone "-" is a filename (stdin), not a flag.
 *
 * GNU also lets options come after operands - `grep foo -i file` works -
 * which it does by shuffling argv. We do the shuffle up front, in one
 * pass, because knowing which letters take a value is enough to tell an
 * option's value from an operand, and doing it once is far easier to be
 * sure of than doing it incrementally.
 */
#include "types.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"

/* 0 none, 1 required, 2 optional, -1 not in the string */
static int short_arg(const char *spec, char c)
{
    if (!spec || c == ':')
        return -1;
    for (const char *p = spec; *p; p++) {
        if (*p != c)
            continue;
        if (p[1] == ':')
            return p[2] == ':' ? 2 : 1;
        return 0;
    }
    return -1;
}

/* GNU accepts any unambiguous prefix of a long option. */
static const lp_lopt_t *long_find(const lp_lopt_t *tab, const char *name,
                                  size_t len, int *ambiguous)
{
    const lp_lopt_t *hit = NULL;
    int matches = 0;

    *ambiguous = 0;
    if (!tab)
        return NULL;
    for (const lp_lopt_t *l = tab; l->name; l++) {
        if (strncmp(l->name, name, len) != 0)
            continue;
        if (strlen(l->name) == len)
            return l;                      /* exact wins outright */
        hit = l;
        matches++;
    }
    if (matches > 1) {
        *ambiguous = 1;
        return NULL;
    }
    return hit;
}

void lp_getopt_init(lp_getopt_t *st, int argc, char **argv,
                    const char *shortopts, const lp_lopt_t *longopts)
{
    lp_getopt_init_ex(st, argc, argv, shortopts, longopts, 0);
}

void lp_getopt_init_ex(lp_getopt_t *st, int argc, char **argv,
                       const char *shortopts, const lp_lopt_t *longopts,
                       int flags)
{
    memset(st, 0, sizeof(*st));
    st->flags     = flags;
    st->argc      = argc;
    st->argv      = argv;
    st->shortopts = shortopts ? shortopts : "";
    st->longopts  = longopts;
    st->ind       = 1;
    st->pos       = 1;
    st->first_operand = argc;

    /* Nothing to shuffle if the caller has one word or none. */
    if (argc < 2)
        return;

    char **opts = malloc(sizeof(char *) * (size_t)argc);
    char **ops  = malloc(sizeof(char *) * (size_t)argc);
    if (!opts || !ops) {                   /* out of memory: no options, all operands */
        free(opts);
        free(ops);
        st->first_operand = 1;
        return;
    }

    int no = 0, np = 0, i = 1;
    bool dashdash = false;
    while (i < argc) {
        const char *a = argv[i];

        if (a[0] != '-' || a[1] == '\0') { /* operand, or "-" for stdin */
            /* `timeout 5 ls -l` - the -l is the command's, not ours. A
             * program that runs another one stops here and hands over
             * everything that follows. */
            if (flags & LP_GETOPT_STOP_AT_OPERAND) {
                while (i < argc) ops[np++] = argv[i++];
                break;
            }
            ops[np++] = argv[i++];
            continue;
        }
        /* `seq 5 -1 1` and `sort -k2 -5` mean a number, not a flag. Only
         * the tools where that is true ask for it. */
        if ((flags & LP_GETOPT_NEGNUM) &&
            ((a[1] >= '0' && a[1] <= '9') ||
             (a[1] == '.' && a[2] >= '0' && a[2] <= '9'))) {
            ops[np++] = argv[i++];
            continue;
        }
        if (a[1] == '-' && a[2] == '\0') { /* "--": the rest are operands */
            i++;
            dashdash = true;
            while (i < argc)
                ops[np++] = argv[i++];
            break;
        }

        opts[no++] = argv[i++];

        if (a[1] == '-') {                 /* --name or --name=value */
            const char *eq = strchr(a + 2, '=');
            if (eq)
                continue;                  /* value came attached */
            int amb;
            const lp_lopt_t *l = long_find(longopts, a + 2, strlen(a + 2), &amb);
            if (l && l->has_arg == 1 && i < argc)
                opts[no++] = argv[i++];    /* value is the next word */
            continue;
        }

        /* A bundle: only the last letter can pull in the next word, and
         * only if nothing followed it inside the bundle. */
        for (const char *p = a + 1; *p; p++) {
            int ha = short_arg(shortopts, *p);
            if (ha <= 0)
                continue;                  /* no value, or unknown - stop later */
            if (p[1] == '\0' && ha == 1 && i < argc)
                opts[no++] = argv[i++];
            break;                         /* rest of the word is the value */
        }
    }

    /* The "--" stays, between the options and the operands. Dropping
     * it left argv one word short of argc, and the last word was then
     * there twice: `grep -- pat file` searched file two times and
     * printed its name on every line. lp_getopt stops at it. */
    for (int k = 0; k < no; k++)
        argv[1 + k] = opts[k];
    if (dashdash)
        argv[1 + no] = (char *)"--";
    for (int k = 0; k < np; k++)
        argv[1 + no + dashdash + k] = ops[k];

    st->first_operand = 1 + no + dashdash;
    free(opts);
    free(ops);
}

int lp_getopt(lp_getopt_t *st)
{
    st->arg     = NULL;
    st->lname   = NULL;
    st->badchar = 0;
    st->badlong = NULL;
    st->ambig   = 0;
    st->missing = 0;

    /* Still inside a bundle like -abc? */
    if (st->cur && *st->cur) {
        char c = *st->cur++;
        int ha = short_arg(st->shortopts, c);
        if (ha < 0) {
            st->badchar = c;
            if (!*st->cur) { st->cur = NULL; st->pos++; }
            return '?';
        }
        if (ha == 0) {
            if (!*st->cur) { st->cur = NULL; st->pos++; }
            return c;
        }
        if (*st->cur) {                    /* -n5 */
            st->arg = st->cur;
        } else if (ha == 1 && st->pos + 1 < st->first_operand) {
            st->arg = st->argv[++st->pos]; /* -n 5 */
        }
        st->cur = NULL;
        st->pos++;
        /* `head -n` with nothing after it. Handing the caller a NULL
         * here is how every one of these commands segfaulted: they pass
         * st->arg straight to strtol. It is the option grammar's job to
         * catch it, once, rather than every command's to remember. */
        if (ha == 1 && !st->arg) {
            st->badchar = c;
            st->missing = 1;
            return '?';
        }
        return c;
    }

    if (st->pos >= st->first_operand) {
        st->ind = st->first_operand;
        return -1;
    }

    const char *a = st->argv[st->pos];
    if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {    /* the "--" left in place above */
        st->pos = st->ind = st->first_operand;
        return -1;
    }

    if (a[0] == '-' && a[1] == '-') {
        const char *name = a + 2;
        const char *eq   = strchr(name, '=');
        size_t len = eq ? (size_t)(eq - name) : strlen(name);
        int amb;
        const lp_lopt_t *l = long_find(st->longopts, name, len, &amb);
        st->pos++;
        if (!l) {
            st->badlong = a;
            st->ambig   = amb;
            return '?';
        }
        st->lname = l->name;
        if (eq) {
            st->arg = eq + 1;
        } else if (l->has_arg == 1 && st->pos < st->first_operand) {
            st->arg = st->argv[st->pos++];
        }
        if (l->has_arg == 1 && !st->arg) {
            st->badlong = a;
            st->missing = 1;
            return '?';
        }
        return l->val;
    }

    st->cur = a + 1;
    return lp_getopt(st);
}

void lp_getopt_err(const char *prog, const lp_getopt_t *st)
{
    if (st->missing && st->badlong) {
        /* GNU prints the name as it was typed, without any "=value". */
        char name[128];
        strlcpy(name, st->badlong, sizeof name);
        char *eq = strchr(name, '=');
        if (eq) *eq = '\0';
        dprintf(STDERR_FILENO, "%s: option '%s' requires an argument\n", prog, name);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return;
    }
    if (st->missing) {
        dprintf(STDERR_FILENO, "%s: option requires an argument -- '%c'\n",
                prog, st->badchar);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return;
    }
    if (st->badlong && st->ambig)
        dprintf(STDERR_FILENO, "%s: option '%s' is ambiguous\n", prog, st->badlong);
    else if (st->badlong)
        dprintf(STDERR_FILENO, "%s: unrecognized option '%s'\n", prog, st->badlong);
    else
        dprintf(STDERR_FILENO, "%s: invalid option -- '%c'\n", prog, st->badchar);
    dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
}
