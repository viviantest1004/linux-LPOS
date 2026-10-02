/* regex.c - the shared regular expression engine.
 *
 * The pattern is compiled to a small instruction program and run with
 * backtracking, the way Russ Cox describes in "Regular Expression
 * Matching: the Virtual Machine Approach". Twelve instructions:
 *
 *   CHAR ANY CLASS   consume one byte
 *   BOL EOL WORDB NWORDB   look around, consume nothing
 *   SAVE             write the current offset into a slot
 *   JMP SPLIT LOOP   control flow
 *   MATCH            done
 *
 * Why it is built this way, decision by decision:
 *
 * Backtracking, not a Thompson simulation. A parallel simulation would
 * be immune to the exponential cases, but it costs a set of live
 * threads per input byte and it makes capture groups much more code.
 * sed and awk need the groups, and a board with 22MB of RAM would
 * rather have the smaller matcher. The exponential cases are handled by
 * a step budget instead (see below).
 *
 * One pass, no syntax tree. The compiler emits instructions as it
 * parses and, when a postfix operator turns up, it inserts a SPLIT in
 * front of the atom it just emitted and relocates the jump targets that
 * moved. That saves the whole node allocator; the price is
 * insert_inst(), which is O(program) and only runs once per operator.
 *
 * Counted repeats {m,n} copy the atom's instructions m..n times, which
 * is why the program has a hard ceiling: a{200}{200} would otherwise
 * ask for 40000 instructions. Past MAX_INST the compile fails with a
 * sentence saying so rather than eating the memory.
 *
 * A STEP BUDGET. A pattern like \(a*\)*b on a line of a's takes
 * exponential time in any backtracker. This code runs on an appliance
 * in a cupboard with nobody at the keyboard, so it counts steps and
 * gives up: past the budget the search reports "no match" and returns.
 * A wrong answer on a pattern written to be pathological is recoverable
 * - the person edits the pattern. A board that has to be power-cycled
 * is not. The same applies when the backtrack stack hits its ceiling.
 *
 * The LOOP instruction is the one addition to Cox's list. A repeat
 * whose body can match nothing - \(a*\)* - would otherwise spin on the
 * spot until the budget ran out and then report a wrong answer for a
 * pattern people do write by accident. LOOP is the back edge of such a
 * loop: it goes round again if the iteration just finished moved
 * forward, and if it did not it fails, which throws that empty
 * iteration away and leaves \1 holding what the last real one matched.
 * It costs one instruction, and only when the body really is nullable.
 *
 * What it does NOT do:
 *
 *   - leftmost-longest. Alternation is first-match, like perl and
 *     unlike POSIX: a|ab on "ab" matches "a". Getting the longest
 *     alternative means running every branch to the end on every
 *     attempt, and nothing here is worth that.
 *   - python's idea of what \1 holds after a repeat whose body matched
 *     nothing last time round. \(a*\)* on "aaa" leaves \1 as "aaa" here,
 *     which is what GNU sed does and what anyone comparing the two
 *     would expect; python leaves it empty.
 *   - backreferences in the pattern. \1 in a sed replacement is sed's
 *     business, not the matcher's; \1 in a pattern is refused with a
 *     message saying so.
 *   - bytes above 127 have no case and belong to no [:alpha:]. There is
 *     no locale in this system, so folding a UTF-8 line would be
 *     guesswork. A class written out as [ㄱㄴ] still works: it matches
 *     the bytes, which is what the person wrote.
 *
 * Text is a NUL terminated string, so no instruction ever matches the
 * NUL: . and [^a] stop at the end of the line rather than running off
 * it.
 */
#include "regex.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

/* ── the instruction set ─────────────────────────────────────────── */

enum {
    I_CHAR, I_ANY, I_CLASS, I_MATCH, I_JMP, I_SPLIT,
    I_SAVE, I_BOL, I_EOL, I_WORDB, I_NWORDB, I_LOOP
};

/* which edge of a word I_WORDB wants, in x */
#define WB_EITHER 0                          /* \b */
#define WB_BEGIN  1                          /* \< */
#define WB_END    2                          /* \> */

typedef struct {
    u8  op;
    u8  ch;                                  /* I_CHAR: the byte */
    s32 x;                                   /* target, slot, class, mode */
    s32 y;                                   /* second target */
} reinst;

typedef struct { s32 pc, sp, jr; } btrack;   /* a pending alternative */
typedef struct { s32 slot, old; } jent;      /* one undone SAVE */

#define MAX_INST   4096                      /* after repeats are expanded */
#define MAX_REG    16                        /* nullable loops in one pattern */
#define ST_MAX     32768                     /* backtrack stack entries */
#define JR_MAX     32768                     /* undo journal entries */

/* 200000 steps is far more than any sane pattern needs against one
 * start position - a 4KB line scanned by .*foo uses a few thousand -
 * and the whole-search ceiling keeps a long line from multiplying that
 * by its own length. Together they are a fraction of a second on a Pi
 * Zero, and they are the difference between a bad answer and a hang. */
#define STEPS_PER_START 200000L
#define STEPS_TOTAL     20000000L

struct lpre {
    reinst *prog;
    int     n, cap;
    u8     *cls;                             /* ncls sets of 32 bytes */
    int     ncls, clscap;
    int     ngroup;
    int     nreg;                            /* LOOP registers used */
    bool    icase;
    int    *save;                            /* RE_MAX_CAPS + nreg slots */
    btrack *st;   int stcap;
    jent   *jr;   int jrcap;
    bool    has_first;                       /* first[] is usable */
    bool    gave_up;                         /* the last search hit the
                                                step budget rather than
                                                finishing */
    u8      first[32];                       /* bytes a match can start on */
};

/* Compile errors that name a character are formatted here. One buffer:
 * a compile that failed has no live regex to keep it for, and nothing
 * in this system compiles two patterns at once. */
static char errbuf[96];

/* ── character kinds, ASCII only ─────────────────────────────────── */

static bool is_upper(int c) { return c >= 'A' && c <= 'Z'; }
static bool is_lower(int c) { return c >= 'a' && c <= 'z'; }
static bool is_alpha(int c) { return is_upper(c) || is_lower(c); }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static bool is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static bool is_blank(int c) { return c == ' ' || c == '\t'; }
static bool is_punct(int c) { return c > ' ' && c < 127 && !is_alnum(c); }
static bool is_word(int c)  { return is_alnum(c) || c == '_'; }
static bool is_xdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static u8 lower(u8 c) { return is_upper(c) ? (u8)(c + 32) : c; }

/* ── 256 bit character sets ──────────────────────────────────────── */

static void set_add(u8 *s, int c)        { s[(c & 255) >> 3] |= (u8)(1 << (c & 7)); }
static bool set_has(const u8 *s, int c)  { return (s[(c & 255) >> 3] >> (c & 7)) & 1; }
static void set_range(u8 *s, int a, int b) { for (; a <= b; a++) set_add(s, a); }
static void set_or(u8 *d, const u8 *s)   { for (int i = 0; i < 32; i++) d[i] |= s[i]; }
static void set_not(u8 *s)               { for (int i = 0; i < 32; i++) s[i] = (u8)~s[i]; }

/* a-z and A-Z become the same set, so icase can be decided once here
 * instead of on every byte of every line */
static void set_fold(u8 *s)
{
    for (int c = 'a'; c <= 'z'; c++) {
        if (set_has(s, c)) set_add(s, c - 32);
        else if (set_has(s, c - 32)) set_add(s, c);
    }
}

/* ── the parser ──────────────────────────────────────────────────── */

/* p_alt and p_atom call each other, so the parser recurses - but only
 * for a group, and there can be at most nine of those, so the depth is
 * capped at ten frames however long the pattern is. Nothing else in
 * this file recurses; the matcher does not recurse at all. */
typedef struct {
    const char *p;
    bool        ere;
    lpre       *re;
    const char *err;
} pst;

static bool p_alt(pst *ps);

static int emit(pst *ps, u8 op, u8 ch, int x, int y)
{
    lpre *re = ps->re;

    if (re->n >= re->cap) {
        int want = re->cap ? re->cap * 2 : 32;
        if (want > MAX_INST) want = MAX_INST;
        if (re->n >= want) {
            ps->err = "the pattern is too big once its {n} repeats are "
                      "expanded - use smaller counts";
            return -1;
        }
        reinst *np = realloc(re->prog, (size_t)want * sizeof *np);
        if (!np) { ps->err = "out of memory compiling the pattern"; return -1; }
        re->prog = np;
        re->cap  = want;
    }
    reinst *in = &re->prog[re->n];
    in->op = op; in->ch = ch; in->x = x; in->y = y;
    return re->n++;
}

/* Put one instruction in front of an already emitted stretch of code.
 * Everything from `at` on shifts up by one, so the jumps that pointed
 * there have to move with it - except the ones that land exactly on
 * `at`, where "there" is ambiguous and the answer depends on where the
 * jump is coming from:
 *
 *   from before at   it means "carry on with the code that starts
 *                    here", and the instruction being inserted is now
 *                    the first of it. The target stays.
 *   from at or after it is inside the stretch being wrapped and means
 *                    that particular instruction - a star's back edge
 *                    to its own SPLIT, say, when a second star wraps
 *                    it. The target moves.
 *
 * Getting that wrong is subtle and stays hidden: x*y* compiled with one
 * blanket rule sends the exit of the first star into the middle of the
 * second one, and only patterns with two adjacent repeats break.
 *
 * Targets of -1 are holes waiting to be patched and stay holes. */
static bool insert_inst(pst *ps, int at, u8 op, u8 ch, int x, int y)
{
    lpre *re = ps->re;

    if (emit(ps, I_MATCH, 0, 0, 0) < 0) return false;   /* room at the end */
    memmove(&re->prog[at + 1], &re->prog[at],
            (size_t)(re->n - 1 - at) * sizeof re->prog[0]);

    for (int i = 0; i < re->n; i++) {
        reinst *in = &re->prog[i];
        int    low = (i < at) ? at + 1 : at;     /* == at: stay, or move */
        if ((in->op == I_JMP || in->op == I_SPLIT) && in->x >= low) in->x++;
        if ((in->op == I_SPLIT || in->op == I_LOOP) && in->y >= low) in->y++;
    }
    re->prog[at].op = op;
    re->prog[at].ch = ch;
    re->prog[at].x  = x;
    re->prog[at].y  = y;
    return true;
}

/* Everything reachable from `start` without consuming a byte.
 *
 * Two questions are asked of it. "Can this fragment match nothing?" -
 * then stop is the end of the fragment and the answer is whether it is
 * reachable. "Which byte can a match start with?" - then `set` collects
 * what every reachable CHAR/CLASS would accept, and *any is set if one
 * of them is ANY or if MATCH is reachable, either of which means no
 * useful answer. Assertions are walked straight through, which can only
 * make both answers more generous, and both are safe that way.
 *
 * It iterates to a fixed point rather than recursing: the program is at
 * most MAX_INST long and this runs once per repeat operator. */
static bool closure(lpre *re, int start, int stop, u8 *set, bool *any)
{
    u8 *seen = calloc((size_t)(stop - start + 1), 1);
    if (!seen) return true;                  /* no memory: assume the worst */

    seen[0] = 1;
    for (bool moved = true; moved; ) {
        moved = false;
        for (int i = start; i < stop; i++) {
            if (!seen[i - start]) continue;
            reinst *in = &re->prog[i];
            int t1 = -1, t2 = -1;

            switch (in->op) {
            case I_SAVE: case I_BOL: case I_EOL:
            case I_WORDB: case I_NWORDB:      t1 = i + 1; break;
            case I_JMP:                       t1 = in->x; break;
            case I_SPLIT:                     t1 = in->x; t2 = in->y; break;
            case I_LOOP:                      t1 = in->y; break;
            case I_MATCH:                     if (any) *any = true; break;
            case I_CHAR:
                if (set) {
                    set_add(set, in->ch);
                    if (re->icase && is_lower(in->ch)) set_add(set, in->ch - 32);
                }
                break;
            case I_CLASS: if (set) set_or(set, re->cls + (size_t)in->x * 32); break;
            case I_ANY:   if (any) *any = true; break;
            }
            if (t1 >= start && t1 <= stop && !seen[t1 - start]) { seen[t1 - start] = 1; moved = true; }
            if (t2 >= start && t2 <= stop && !seen[t2 - start]) { seen[t2 - start] = 1; moved = true; }
        }
    }
    bool empty = seen[stop - start] != 0;
    free(seen);
    return empty;
}

/* Lift out a stretch of code so it can be stamped down again. Targets
 * are made relative to the fragment; a target equal to its length is
 * "just past the end", which is where the next copy begins. */
static reinst *frag_save(pst *ps, int start, int *len)
{
    int L = ps->re->n - start;
    reinst *f = malloc((size_t)(L ? L : 1) * sizeof *f);

    if (!f) { ps->err = "out of memory compiling the pattern"; return NULL; }
    for (int i = 0; i < L; i++) {
        f[i] = ps->re->prog[start + i];
        if (f[i].op == I_JMP || f[i].op == I_SPLIT) f[i].x -= start;
        if (f[i].op == I_SPLIT || f[i].op == I_LOOP) f[i].y -= start;
    }
    *len = L;
    return f;
}

static bool frag_emit(pst *ps, const reinst *f, int L)
{
    int dst = ps->re->n;

    for (int i = 0; i < L; i++) {
        reinst in = f[i];
        if (in.op == I_JMP || in.op == I_SPLIT) in.x += dst;
        if (in.op == I_SPLIT || in.op == I_LOOP) in.y += dst;
        if (emit(ps, in.op, in.ch, in.x, in.y) < 0) return false;
    }
    return true;
}

/* x* over the code already emitted at [start, n):
 *
 *   start: SPLIT body, out          out: ...
 *    body: [SAVE reg]  the guard, only when the body can match nothing
 *          ...
 *          LOOP reg, start  or  JMP start
 *
 * With the guard the back edge is a LOOP, which fails instead of going
 * round when the iteration matched nothing. Failing rather than falling
 * out matters: it makes the machine undo that iteration's SAVEs, so
 * \(a*\)* on "aaa" leaves \1 as "aaa" and not as the empty string the
 * last spin round the loop would have put there.
 */
static bool emit_star(pst *ps, int start)
{
    lpre *re = ps->re;
    bool  guard = closure(re, start, re->n, NULL, NULL);
    int   reg = 0;

    if (guard) {
        if (re->nreg >= MAX_REG) {
            ps->err = "more than 16 repeats in this pattern have a body "
                      "that can match nothing - take the * off the parts "
                      "that are already optional";
            return false;
        }
        reg = RE_MAX_CAPS + re->nreg++;
        if (!insert_inst(ps, start, I_SAVE, 0, reg, 0)) return false;
    }
    if (!insert_inst(ps, start, I_SPLIT, 0, start + 1, -1)) return false;
    if (guard) { if (emit(ps, I_LOOP, 0, reg, start) < 0) return false; }
    else       { if (emit(ps, I_JMP, 0, start, 0) < 0) return false; }

    re->prog[start].y = re->n;
    return true;
}

static bool emit_opt(pst *ps, int start)
{
    if (!insert_inst(ps, start, I_SPLIT, 0, start + 1, -1)) return false;
    ps->re->prog[start].y = ps->re->n;
    return true;
}

/* x+ is a back edge over the body, except when the body can match
 * nothing: then it becomes x x*, which costs a second copy of the body
 * but gets the loop guard for free from emit_star. */
static bool emit_plus(pst *ps, int start)
{
    if (!closure(ps->re, start, ps->re->n, NULL, NULL)) {
        int s = emit(ps, I_SPLIT, 0, start, 0);
        if (s < 0) return false;
        ps->re->prog[s].y = s + 1;
        return true;
    }
    int L, cs;
    reinst *f = frag_save(ps, start, &L);
    if (!f) return false;
    cs = ps->re->n;
    bool ok = frag_emit(ps, f, L) && emit_star(ps, cs);
    free(f);
    return ok;
}

/* x{m,n}. n < 0 means {m,}. The atom is copied out, the original
 * dropped, and the copies stamped back down. */
static bool emit_rep(pst *ps, int start, int m, int n)
{
    int L;
    reinst *f = frag_save(ps, start, &L);
    if (!f) return false;

    ps->re->n = start;
    bool ok = true;

    if (n < 0) {
        if (m == 0) {
            int cs = ps->re->n;
            ok = frag_emit(ps, f, L) && emit_star(ps, cs);
        } else {
            for (int i = 0; ok && i < m - 1; i++) ok = frag_emit(ps, f, L);
            int cs = ps->re->n;
            ok = ok && frag_emit(ps, f, L) && emit_plus(ps, cs);
        }
    } else {
        for (int i = 0; ok && i < m; i++) ok = frag_emit(ps, f, L);

        /* The optional tail. Each copy gets a SPLIT in front of it that
         * can jump straight to the end; the ends are not known yet, so
         * the splits are threaded onto a list through their own y field
         * and patched when the end is. */
        int chain = -1;
        for (int i = m; ok && i < n; i++) {
            int s = emit(ps, I_SPLIT, 0, 0, chain);
            if (s < 0) { ok = false; break; }
            ps->re->prog[s].x = s + 1;
            chain = s;
            ok = frag_emit(ps, f, L);
        }
        while (ok && chain >= 0) {
            int nx = ps->re->prog[chain].y;
            ps->re->prog[chain].y = ps->re->n;
            chain = nx;
        }
    }
    free(f);
    return ok;
}

/* ── escapes ─────────────────────────────────────────────────────── */

/* the literal byte \c stands for, or -1 if c names a set */
static int esc_char(char c)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case 'e': return 27;
    case 'w': case 'W': case 's': case 'S': case 'd': case 'D': return -1;
    default:  return (u8)c;                  /* \. \* \\ and the rest */
    }
}

static bool esc_set(char c, u8 *set)
{
    u8 tmp[32];
    int kind = (c == 'w' || c == 'W') ? 0 : (c == 's' || c == 'S') ? 1 :
               (c == 'd' || c == 'D') ? 2 : -1;

    if (kind < 0) return false;
    memset(tmp, 0, sizeof tmp);
    for (int i = 1; i < 256; i++)
        if (kind == 0 ? is_word(i) : kind == 1 ? is_space(i) : is_digit(i))
            set_add(tmp, i);
    if (is_upper(c)) { set_not(tmp); tmp[0] &= (u8)~1; }
    set_or(set, tmp);
    return true;
}

/* ── [...] ───────────────────────────────────────────────────────── */

static int posix_kind(const char *n, int len)
{
    static const char *names[] = { "alpha", "digit", "alnum", "space",
                                   "upper", "lower", "punct", "xdigit",
                                   "blank", 0 };
    for (int i = 0; names[i]; i++)
        if ((int)strlen(names[i]) == len && strncmp(names[i], n, (size_t)len) == 0)
            return i;
    return -1;
}

static bool posix_has(int k, int c)
{
    switch (k) {
    case 0: return is_alpha(c);
    case 1: return is_digit(c);
    case 2: return is_alnum(c);
    case 3: return is_space(c);
    case 4: return is_upper(c);
    case 5: return is_lower(c);
    case 6: return is_punct(c);
    case 7: return is_xdigit(c);
    default: return is_blank(c);
    }
}

static int cls_new(pst *ps, const u8 *set)
{
    lpre *re = ps->re;

    if (re->ncls >= re->clscap) {
        int want = re->clscap ? re->clscap * 2 : 4;
        u8 *nc = realloc(re->cls, (size_t)want * 32);
        if (!nc) { ps->err = "out of memory compiling the pattern"; return -1; }
        re->cls = nc;
        re->clscap = want;
    }
    memcpy(re->cls + (size_t)re->ncls * 32, set, 32);
    return re->ncls++;
}

/* One member of a bracket: a byte, or -1 when it added a whole set of
 * its own ([:digit:], \w) and there is nothing to make a range with. */
static int class_item(pst *ps, u8 *set)
{
    const char *p = ps->p;

    if (p[0] == '[' && p[1] == ':') {
        const char *e = p + 2;
        while (*e && *e != ':') e++;
        if (e[0] != ':' || e[1] != ']') {
            ps->err = "a [: name :] inside brackets has no closing :]";
            return -2;
        }
        int k = posix_kind(p + 2, (int)(e - (p + 2)));
        if (k < 0) {
            snprintf(errbuf, sizeof errbuf,
                     "[:%.*s:] is not a class name - try [:alpha:] [:digit:] "
                     "[:space:]", (int)(e - (p + 2)), p + 2);
            ps->err = errbuf;
            return -2;
        }
        for (int c = 1; c < 256; c++) if (posix_has(k, c)) set_add(set, c);
        ps->p = e + 2;
        return -1;
    }
    /* Escapes inside brackets are not POSIX, where [\t] means backslash
     * or t. Nobody in this system has ever meant that, and every sed
     * script written here has meant the tab. */
    if (p[0] == '\\' && p[1]) {
        char c = p[1];
        ps->p = p + 2;
        if (esc_set(c, set)) return -1;
        return esc_char(c);
    }
    ps->p = p + 1;
    return (u8)p[0];
}

static bool p_class(pst *ps)
{
    u8   set[32];
    bool neg = false;

    memset(set, 0, sizeof set);
    ps->p++;                                 /* past [ */
    if (*ps->p == '^') { neg = true; ps->p++; }

    for (bool first = true; ; first = false) {
        if (*ps->p == 0) {
            ps->err = "unmatched [ in the pattern - add a closing ]";
            return false;
        }
        if (*ps->p == ']' && !first) break;

        int lo = class_item(ps, set);
        if (lo == -2) return false;
        if (lo == -1) continue;              /* [:digit:] or \w, no range */

        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            ps->p++;
            int hi = class_item(ps, set);
            if (hi == -2) return false;
            if (hi == -1) {
                ps->err = "a range inside brackets needs plain characters "
                          "on both sides";
                return false;
            }
            if (hi < lo) {
                snprintf(errbuf, sizeof errbuf,
                         "the range %c-%c runs backwards", lo, hi);
                ps->err = errbuf;
                return false;
            }
            set_range(set, lo, hi);
        } else {
            set_add(set, lo);
        }
    }
    ps->p++;                                 /* past ] */

    /* Fold first, negate second: with -i, [^a] must also refuse A. */
    if (ps->re->icase) set_fold(set);
    if (neg) set_not(set);
    set[0] &= (u8)~1;                        /* never the end of the text */

    int ci = cls_new(ps, set);
    if (ci < 0) return false;
    return emit(ps, I_CLASS, 0, ci, 0) >= 0;
}

/* ── operators, spelt differently in BRE and ERE ──────────────────── */

static int at_open(pst *ps)                  /* length of ( or \( , else 0 */
{
    if (ps->ere) return ps->p[0] == '(' ? 1 : 0;
    return (ps->p[0] == '\\' && ps->p[1] == '(') ? 2 : 0;
}

static int at_close(pst *ps)
{
    if (ps->ere) return ps->p[0] == ')' ? 1 : 0;
    return (ps->p[0] == '\\' && ps->p[1] == ')') ? 2 : 0;
}

static int at_bar(pst *ps)
{
    if (ps->ere) return ps->p[0] == '|' ? 1 : 0;
    return (ps->p[0] == '\\' && ps->p[1] == '|') ? 2 : 0;
}

/* Is the cursor on a postfix repeat? *op gets * + ? or { . */
static bool at_postfix(pst *ps, char *op)
{
    char a = ps->p[0], b = ps->p[1];

    if (ps->ere) {
        if (a == '*' || a == '+' || a == '?') { *op = a; return true; }
        if (a == '{' && is_digit((u8)b))      { *op = '{'; return true; }
        return false;
    }
    if (a == '*') { *op = '*'; return true; }
    if (a == '\\' && (b == '+' || b == '?')) { *op = b; return true; }
    if (a == '\\' && b == '{')               { *op = '{'; return true; }
    return false;
}

/* In BRE a $ is only an anchor at the very end of the pattern or just
 * before \) or \| . Anywhere else it is a dollar sign, which is what
 * anyone writing s/a$b/ meant. */
static bool dollar_anchors(pst *ps)
{
    if (ps->ere) return true;
    if (ps->p[1] == 0) return true;
    return ps->p[1] == '\\' && (ps->p[2] == ')' || ps->p[2] == '|');
}

/* ── atoms ───────────────────────────────────────────────────────── */

static bool p_atom(pst *ps, bool first)
{
    int  k;
    char c = ps->p[0];

    if ((k = at_open(ps)) != 0) {
        ps->p += k;
        if (ps->re->ngroup + 1 >= RE_MAX_GROUPS) {
            ps->err = "the pattern has more than 9 groups";
            return false;
        }
        int g = ++ps->re->ngroup;
        if (emit(ps, I_SAVE, 0, 2 * g, 0) < 0) return false;
        if (!p_alt(ps)) return false;
        if ((k = at_close(ps)) == 0) {
            ps->err = ps->ere ? "unmatched ( in the pattern"
                              : "unmatched \\( in the pattern";
            return false;
        }
        ps->p += k;
        return emit(ps, I_SAVE, 0, 2 * g + 1, 0) >= 0;
    }
    if (c == '.') { ps->p++; return emit(ps, I_ANY, 0, 0, 0) >= 0; }
    if (c == '[') return p_class(ps);

    if (c == '^' && (ps->ere || first)) {
        ps->p++;
        return emit(ps, I_BOL, 0, 0, 0) >= 0;
    }
    if (c == '$' && dollar_anchors(ps)) {
        ps->p++;
        return emit(ps, I_EOL, 0, 0, 0) >= 0;
    }
    if (c == '\\') {
        char e = ps->p[1];
        if (e == 0) {
            ps->err = "the pattern ends with a backslash";
            return false;
        }
        ps->p += 2;
        if (e == '<') return emit(ps, I_WORDB, 0, WB_BEGIN, 0) >= 0;
        if (e == '>') return emit(ps, I_WORDB, 0, WB_END, 0) >= 0;
        if (e == 'b') return emit(ps, I_WORDB, 0, WB_EITHER, 0) >= 0;
        if (e == 'B') return emit(ps, I_NWORDB, 0, 0, 0) >= 0;
        if (e >= '1' && e <= '9') {
            snprintf(errbuf, sizeof errbuf,
                     "\\%c in a pattern would be a backreference, which this "
                     "matcher does not do", e);
            ps->err = errbuf;
            return false;
        }
        u8 set[32];
        memset(set, 0, sizeof set);
        if (esc_set(e, set)) {
            if (ps->re->icase) set_fold(set);
            int ci = cls_new(ps, set);
            if (ci < 0) return false;
            return emit(ps, I_CLASS, 0, ci, 0) >= 0;
        }
        int lit = esc_char(e);
        return emit(ps, I_CHAR, ps->re->icase ? lower((u8)lit) : (u8)lit, 0, 0) >= 0;
    }
    ps->p++;
    return emit(ps, I_CHAR, ps->re->icase ? lower((u8)c) : (u8)c, 0, 0) >= 0;
}

/* {m} {m,} {m,n} - the cursor is on { or \{ */
static bool p_bounds(pst *ps, int start)
{
    const char *p = ps->p + (ps->ere ? 1 : 2);
    int m = 0, n;

    if (!is_digit((u8)*p)) {
        ps->err = "a {} repeat needs a number, as in a{2} or a{2,5}";
        return false;
    }
    for (; is_digit((u8)*p); p++) {
        m = m * 10 + (*p - '0');
        if (m > 255) { ps->err = "a {} repeat above 255 is too many"; return false; }
    }
    n = m;
    if (*p == ',') {
        p++;
        if (is_digit((u8)*p)) {
            for (n = 0; is_digit((u8)*p); p++) {
                n = n * 10 + (*p - '0');
                if (n > 255) { ps->err = "a {} repeat above 255 is too many"; return false; }
            }
        } else {
            n = -1;
        }
    }
    if (ps->ere ? (*p != '}') : (p[0] != '\\' || p[1] != '}')) {
        ps->err = ps->ere ? "unmatched { in the pattern - write \\{ for a "
                            "plain brace"
                          : "unmatched \\{ in the pattern";
        return false;
    }
    if (n >= 0 && n < m) {
        snprintf(errbuf, sizeof errbuf,
                 "the repeat {%d,%d} counts backwards", m, n);
        ps->err = errbuf;
        return false;
    }
    ps->p = p + (ps->ere ? 1 : 2);
    return emit_rep(ps, start, m, n);
}

static bool p_postfix(pst *ps, int start)
{
    char op;

    while (at_postfix(ps, &op)) {
        if (op == '{') { if (!p_bounds(ps, start)) return false; continue; }
        ps->p += (ps->ere || op == '*') ? 1 : 2;
        if (op == '*') { if (!emit_star(ps, start)) return false; }
        else if (op == '+') { if (!emit_plus(ps, start)) return false; }
        else if (!emit_opt(ps, start)) return false;
    }
    return true;
}

static bool p_concat(pst *ps)
{
    bool first = true;

    for (;;) {
        char op;
        if (*ps->p == 0 || at_bar(ps) || at_close(ps)) return true;

        if (first && at_postfix(ps, &op)) {
            /* In BRE a * with nothing in front of it is a star, and
             * plenty of patterns rely on that. Everything else here is
             * a mistake worth naming. */
            if (ps->ere || op != '*') {
                snprintf(errbuf, sizeof errbuf,
                         "nothing for %c to repeat", op);
                ps->err = errbuf;
                return false;
            }
        }
        int astart = ps->re->n;
        if (!p_atom(ps, first)) return false;
        if (!p_postfix(ps, astart)) return false;
        first = false;
    }
}

/* a|b|c becomes SPLIT a, (SPLIT b, c). Each finished branch gets a
 * SPLIT pushed in front of it and a JMP to the end appended; the jumps
 * cannot be patched until the last branch is parsed, so they are strung
 * together through their own target field until then. */
static bool p_alt(pst *ps)
{
    int branch = ps->re->n, jchain = -1, k;

    if (!p_concat(ps)) return false;
    while ((k = at_bar(ps)) != 0) {
        ps->p += k;
        if (!insert_inst(ps, branch, I_SPLIT, 0, branch + 1, -1)) return false;
        int j = emit(ps, I_JMP, 0, jchain, 0);
        if (j < 0) return false;
        jchain = j;
        ps->re->prog[branch].y = ps->re->n;      /* on to the next branch */
        branch = ps->re->n;
        if (!p_concat(ps)) return false;
    }
    while (jchain >= 0) {
        int nx = ps->re->prog[jchain].x;
        ps->re->prog[jchain].x = ps->re->n;
        jchain = nx;
    }
    return true;
}

/* ── compile ─────────────────────────────────────────────────────── */

lpre *re_compile(const char *pattern, bool ere, bool icase, const char **err)
{
    static const char *sink;
    if (!err) err = &sink;
    *err = NULL;

    if (!pattern) { *err = "there is no pattern to compile"; return NULL; }

    lpre *re = calloc(1, sizeof *re);
    if (!re) { *err = "out of memory compiling the pattern"; return NULL; }
    re->icase = icase;

    pst ps;
    ps.p = pattern; ps.ere = ere; ps.re = re; ps.err = NULL;

    if (emit(&ps, I_SAVE, 0, 0, 0) < 0) goto fail;
    if (!p_alt(&ps)) goto fail;
    if (*ps.p) {
        ps.err = ere ? "unmatched ) in the pattern"
                     : "unmatched \\) in the pattern";
        goto fail;
    }
    if (emit(&ps, I_SAVE, 0, 1, 0) < 0) goto fail;
    if (emit(&ps, I_MATCH, 0, 0, 0) < 0) goto fail;

    re->save = malloc((size_t)(RE_MAX_CAPS + re->nreg) * sizeof(int));
    if (!re->save) { ps.err = "out of memory compiling the pattern"; goto fail; }

    /* Which byte can a match begin with? When the answer is a short
     * list, re_search skips start positions with a byte test instead of
     * a run of the program, which is most of the cost of scanning a
     * long line for a pattern that is not there. */
    bool any = false;
    memset(re->first, 0, sizeof re->first);
    closure(re, 0, re->n, re->first, &any);
    re->has_first = !any;
    if (re->has_first) {
        bool empty = true;
        for (int i = 0; i < 32; i++) if (re->first[i]) empty = false;
        if (empty) re->has_first = false;
    }
    return re;

fail:
    *err = ps.err ? ps.err : "the pattern is not valid";
    re_free(re);
    return NULL;
}

void re_free(lpre *re)
{
    if (!re) return;
    free(re->prog);
    free(re->cls);
    free(re->save);
    free(re->st);
    free(re->jr);
    free(re);
}

int re_ngroups(const lpre *re)
{
    return re ? re->ngroup : 0;
}

/* ── the machine ─────────────────────────────────────────────────── */

static bool stack_room(lpre *re, int need)
{
    if (need <= re->stcap) return true;
    if (need > ST_MAX) return false;
    int want = re->stcap ? re->stcap * 2 : 256;
    while (want < need) want *= 2;
    if (want > ST_MAX) want = ST_MAX;
    btrack *ns = realloc(re->st, (size_t)want * sizeof *ns);
    if (!ns) return false;
    re->st = ns;
    re->stcap = want;
    return true;
}

static bool journal_room(lpre *re, int need)
{
    if (need <= re->jrcap) return true;
    if (need > JR_MAX) return false;
    int want = re->jrcap ? re->jrcap * 2 : 256;
    while (want < need) want *= 2;
    if (want > JR_MAX) want = JR_MAX;
    jent *nj = realloc(re->jr, (size_t)want * sizeof *nj);
    if (!nj) return false;
    re->jr = nj;
    re->jrcap = want;
    return true;
}

/* Run the program from one start offset. Returns true on a match, with
 * the save slots left holding the offsets. `allow` is how many steps
 * this attempt may take; *used says how many it took, so the caller can
 * keep a ceiling on the whole search.
 *
 * There is no recursion here at all. The pending alternatives live on
 * re->st and the SAVE slots that have to be put back when one is taken
 * live on re->jr, so a line of any length uses a fixed amount of C
 * stack. Both arrays have a ceiling; reaching it ends the attempt the
 * same way the step budget does. */
static bool run(lpre *re, const char *text, int len, int sp0, bool notbol,
                long allow, long *used)
{
    int *sv  = re->save;
    int  nsv = RE_MAX_CAPS + re->nreg;
    int  nst = 0, njr = 0;
    int  pc  = 0, sp = sp0;
    long steps = 0;

    for (int i = 0; i < nsv; i++) sv[i] = -1;

    for (;;) {
        if (++steps > allow) break;

        reinst *in = &re->prog[pc];
        bool    ok = false;

        switch (in->op) {
        case I_CHAR: {
            u8 c = (u8)text[sp];
            if (sp < len && (re->icase ? lower(c) : c) == in->ch) { sp++; pc++; ok = true; }
            break;
        }
        case I_ANY:
            if (sp < len) { sp++; pc++; ok = true; }
            break;
        case I_CLASS:
            if (sp < len && set_has(re->cls + (size_t)in->x * 32, (u8)text[sp])) {
                sp++; pc++; ok = true;
            }
            break;
        case I_BOL:
            if (sp == 0 && !notbol) { pc++; ok = true; }
            break;
        case I_EOL:
            if (sp == len) { pc++; ok = true; }
            break;
        case I_WORDB:
        case I_NWORDB: {
            bool a = sp > 0   && is_word((u8)text[sp - 1]);
            bool b = sp < len && is_word((u8)text[sp]);
            bool hit;
            if (in->op == I_NWORDB)     hit = (a == b);
            else if (in->x == WB_BEGIN) hit = (!a && b);
            else if (in->x == WB_END)   hit = (a && !b);
            else                        hit = (a != b);
            if (hit) { pc++; ok = true; }
            break;
        }
        case I_SAVE:
            if (!journal_room(re, njr + 1)) goto spent;
            re->jr[njr].slot = in->x;
            re->jr[njr].old  = sv[in->x];
            njr++;
            sv[in->x] = sp;
            pc++; ok = true;
            break;
        case I_JMP:
            pc = in->x; ok = true;
            break;
        case I_SPLIT:
            if (!stack_room(re, nst + 1)) goto spent;
            re->st[nst].pc = in->y;
            re->st[nst].sp = sp;
            re->st[nst].jr = njr;
            nst++;
            pc = in->x; ok = true;
            break;
        case I_LOOP:
            /* nothing consumed this time round: fail, so the SAVEs the
             * empty iteration made are undone with it */
            if (sv[in->x] != sp) { pc = in->y; ok = true; }
            break;
        case I_MATCH:
            *used = steps;
            return true;
        }
        if (ok) continue;

        if (nst == 0) break;
        btrack b = re->st[--nst];
        while (njr > b.jr) { njr--; sv[re->jr[njr].slot] = re->jr[njr].old; }
        pc = b.pc;
        sp = b.sp;
    }
spent:
    *used = steps;
    return false;
}

bool re_search(lpre *re, const char *text, int from, bool notbol, int *caps)
{
    if (!re || !text || !caps) return false;

    /* Cleared here so that re_gave_up() answers about THIS search and not
     * about one three lines ago. A caller that checks it after every
     * search - which is the only useful way to check it - would otherwise
     * keep reporting a budget that was spent on a different line. */
    re->gave_up = false;

    int  len = (int)strlen(text);
    long left = STEPS_TOTAL;

    if (from < 0) from = 0;
    if (from > len) return false;

    for (int i = from; i <= len; i++) {
        if (re->has_first) {
            while (i < len && !set_has(re->first, (u8)text[i])) i++;
            if (i >= len) return false;      /* a match needs a byte here */
        }
        long allow = left < STEPS_PER_START ? left : STEPS_PER_START;
        long used  = 0;

        if (allow <= 0) { re->gave_up = true; return false; }
        if (run(re, text, len, i, notbol, allow, &used)) {
            for (int k = 0; k < RE_MAX_CAPS; k++) caps[k] = re->save[k];
            return true;
        }
        left -= used;
        if (used >= allow)
            re->gave_up = true;              /* this start position ran out */
    }
    return false;
}

bool re_gave_up(const lpre *re)
{
    return re && re->gave_up;
}
