/* var.c - shell variables, arrays, local scopes and the environment.
 * Part of sh.c; see sh.h.
 *
 * The old shell kept every variable in the process environment, which is
 * why `x=1` was visible to every program it ran and why nothing could be
 * readonly, local to a function or unexported. Here variables live in
 * the shell's own table and the environment handed to a program is
 * built from the exported ones at the moment it is started - which is
 * what POSIX describes and what every script assumes without saying so.
 *
 * ── Scopes ──
 *
 * `local x` and the temporary `x=1 cmd` both work the same way: the
 * variable's current state is copied onto a save stack and restored when
 * the function returns or the command finishes. That is dynamic scoping,
 * the same as dash and bash: a function called from a function sees its
 * caller's locals. A local that is not given a value keeps the value it
 * had outside, which is dash's rule and the one Debian's scripts are
 * tested against (bash would make it empty). */

#define VHASH 512
static var_t *vtab[VHASH];
static char **env_cache;
static bool   env_dirty = true;
static bool   getopts_reset;        /* OPTIND was assigned */
static u64    rand_state;
static s64    seconds_base;

static unsigned vhash(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (u8)s[i]) * 16777619u;
    return h % VHASH;
}

static var_t *var_lookup_n(const char *name, size_t n)
{
    for (var_t *v = vtab[vhash(name, n)]; v; v = v->next)
        if (strncmp(v->name, name, n) == 0 && v->name[n] == '\0')
            return v;
    return NULL;
}

static var_t *var_lookup(const char *name)
{
    return var_lookup_n(name, strlen(name));
}

static var_t *var_create(const char *name, size_t n)
{
    var_t *v = xcalloc(sizeof *v);
    v->name = xstrndup(name, n);
    v->flags = V_UNSET;
    unsigned h = vhash(name, n);
    v->next = vtab[h];
    vtab[h] = v;
    return v;
}

static void var_free_value(var_t *v)
{
    xfree(v->val);
    v->val = NULL;
    if (v->arr) {
        for (int i = 0; i < v->arrn; i++)
            xfree(v->arr[i]);
        xfree(v->arr);
        v->arr = NULL;
        v->arrn = 0;
    }
}

static void var_remove(var_t *v)
{
    unsigned h = vhash(v->name, strlen(v->name));
    for (var_t **pp = &vtab[h]; *pp; pp = &(*pp)->next) {
        if (*pp == v) {
            *pp = v->next;
            break;
        }
    }
    var_free_value(v);
    xfree(v->name);
    xfree(v);
}

/* The special variables whose value is worked out when read. */
static const char *var_special(var_t *v)
{
    static char buf[32];
    if (strcmp(v->name, "RANDOM") == 0) {
        rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return itoa_s((long long)((rand_state >> 33) & 0x7FFF), buf);
    }
    if (strcmp(v->name, "SECONDS") == 0)
        return itoa_s((long long)((now_ms() - seconds_base) / 1000), buf);
    if (strcmp(v->name, "LINENO") == 0)
        return itoa_s(cmd_lineno > 0 ? cmd_lineno : input_lineno(), buf);
    return v->val;
}

static const char *var_get(const char *name)
{
    var_t *v = var_lookup(name);
    if (!v || (v->flags & V_UNSET))
        return NULL;
    if (v->flags & V_SPECIAL)
        return var_special(v);
    if (v->flags & V_ARRAY)
        return v->arrn > 0 ? v->arr[0] : NULL;
    return v->val;
}

/* The side effects a few names have when they change. */
static void var_changed(const char *name)
{
    switch (name[0]) {
    case 'P':
        if (strcmp(name, "PATH") == 0)
            path_changed();
        break;
    case 'O':
        if (strcmp(name, "OPTIND") == 0)
            getopts_reset = true;
        break;
    case 'R':
        if (strcmp(name, "RANDOM") == 0) {
            const char *s = var_lookup(name)->val;
            rand_state = s ? (u64)strtoll(s, NULL, 10) : 0;
        }
        break;
    case 'S':
        if (strcmp(name, "SECONDS") == 0) {
            const char *s = var_lookup(name)->val;
            seconds_base = now_ms() - (s ? strtoll(s, NULL, 10) : 0) * 1000;
        }
        break;
    }
}

static bool var_set_n(const char *name, size_t nlen, const char *val,
                      int flags)
{
    var_t *v = var_lookup_n(name, nlen);
    if (!v)
        v = var_create(name, nlen);
    if (v->flags & V_READONLY) {
        sh_warn("%s: readonly variable", v->name);
        return false;
    }
    if (val) {
        char *copy = xstrdup(val);
        if (v->flags & V_ARRAY) {
            /* x=value on an array sets element 0, as in bash */
            if (v->arrn == 0) {
                v->arr = xcalloc(sizeof(char *));
                v->arrn = 1;
            }
            xfree(v->arr[0]);
            v->arr[0] = copy;
        } else {
            xfree(v->val);
            v->val = copy;
        }
        v->flags &= ~V_UNSET;
    }
    v->flags |= flags & (V_EXPORT | V_READONLY | V_INTEGER);
    if (opt[O_allexport] && val)
        v->flags |= V_EXPORT;
    if (v->flags & V_EXPORT)
        env_dirty = true;
    var_changed(v->name);
    return true;
}

static bool var_set(const char *name, const char *val, int flags)
{
    return var_set_n(name, strlen(name), val, flags);
}

static bool var_unset(const char *name)
{
    var_t *v = var_lookup(name);
    if (!v)
        return true;
    if (v->flags & V_READONLY) {
        sh_warn("%s: readonly variable", name);
        return false;
    }
    if (v->flags & V_EXPORT)
        env_dirty = true;
    if (strcmp(name, "PATH") == 0)
        path_changed();
    var_remove(v);
    return true;
}

/* ── arrays ────────────────────────────────────────────────────────── *
 *
 * Indexed arrays only, kept dense with NULL for an element that was
 * never set. That is enough for the arrays people write by hand -
 * a=(x y z), a[3]=w, "${a[@]}" - which is what they were asked for. */

static void var_make_array(var_t *v)
{
    if (v->flags & V_ARRAY)
        return;
    v->flags |= V_ARRAY;
    if (v->val) {
        v->arr = xcalloc(sizeof(char *));
        v->arr[0] = v->val;
        v->arrn = 1;
        v->val = NULL;
    }
}

static const char *var_elem(var_t *v, long long idx)
{
    if (!v || (v->flags & V_UNSET))
        return NULL;
    if (!(v->flags & V_ARRAY))
        return idx == 0 ? ((v->flags & V_SPECIAL) ? var_special(v) : v->val)
                        : NULL;
    if (idx < 0)
        idx += v->arrn;
    if (idx < 0 || idx >= v->arrn)
        return NULL;
    return v->arr[idx];
}

static bool var_set_elem(const char *name, long long idx, const char *val)
{
    var_t *v = var_lookup(name);
    if (!v)
        v = var_create(name, strlen(name));
    if (v->flags & V_READONLY) {
        sh_warn("%s: readonly variable", name);
        return false;
    }
    var_make_array(v);
    if (idx < 0)
        idx += v->arrn;
    if (idx < 0 || idx > 1000000) {
        sh_warn("%s: bad array subscript", name);
        return false;
    }
    if (idx >= v->arrn) {
        v->arr = xrealloc(v->arr, (size_t)(idx + 1) * sizeof(char *));
        for (int i = v->arrn; i <= idx; i++)
            v->arr[i] = NULL;
        v->arrn = (int)idx + 1;
    }
    xfree(v->arr[idx]);
    v->arr[idx] = val ? xstrdup(val) : NULL;
    v->flags &= ~V_UNSET;
    return true;
}

static bool var_set_array(const char *name, strvec_t *vals, bool append)
{
    var_t *v = var_lookup(name);
    if (!v)
        v = var_create(name, strlen(name));
    if (v->flags & V_READONLY) {
        sh_warn("%s: readonly variable", name);
        return false;
    }
    if (!append) {
        var_free_value(v);
        v->flags |= V_ARRAY;
    } else {
        var_make_array(v);
    }
    int base = v->arrn;
    v->arr = xrealloc(v->arr, (size_t)(base + vals->n + 1) * sizeof(char *));
    for (int i = 0; i < vals->n; i++)
        v->arr[base + i] = xstrdup(vals->v[i]);
    v->arrn = base + vals->n;
    v->flags &= ~V_UNSET;
    return true;
}

/* ── the environment ──────────────────────────────────────────────── */

static void var_import_env(char **envp)
{
    if (!envp)
        return;
    for (char **e = envp; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e)
            continue;
        size_t n = (size_t)(eq - *e);
        bool ok = is_name_start((u8)**e);
        for (size_t i = 1; ok && i < n; i++)
            ok = is_name_char((u8)(*e)[i]);
        if (!ok)
            continue;       /* not a name the shell can hold; drop it */
        var_set_n(*e, n, eq + 1, V_EXPORT);
    }
}

static char **env_build(void)
{
    if (!env_dirty && env_cache)
        return env_cache;
    if (env_cache) {
        for (char **e = env_cache; *e; e++)
            xfree(*e);
        xfree(env_cache);
    }
    int n = 0;
    for (int h = 0; h < VHASH; h++)
        for (var_t *v = vtab[h]; v; v = v->next)
            if ((v->flags & V_EXPORT) && !(v->flags & V_UNSET))
                n++;
    env_cache = xmalloc((size_t)(n + 1) * sizeof(char *));
    int i = 0;
    for (int h = 0; h < VHASH; h++) {
        for (var_t *v = vtab[h]; v; v = v->next) {
            if (!(v->flags & V_EXPORT) || (v->flags & V_UNSET))
                continue;
            const char *val = var_get(v->name);
            if (!val)
                continue;
            strbuf_t b = {0};
            sb_puts(&b, v->name);
            sb_putc(&b, '=');
            sb_puts(&b, val);
            env_cache[i++] = sb_take(&b);
        }
    }
    env_cache[i] = NULL;
    env_dirty = false;
    return env_cache;
}

static const char *ifs_value(void)
{
    var_t *v = var_lookup("IFS");
    if (!v || (v->flags & V_UNSET) || !v->val)
        return " \t\n";
    return v->val;
}

/* ── saving and restoring: local and x=1 cmd ───────────────────────── */

typedef struct {
    char  *name;
    var_t *old;         /* a detached copy; NULL if it did not exist */
    int    frame;       /* which function call made it */
} vsave_t;

static vsave_t *vsaves;
static int      nvsaves, capvsaves;
static int      vframe;     /* bumped for every scope */

static var_t *var_snapshot(var_t *v)
{
    if (!v)
        return NULL;
    var_t *c = xcalloc(sizeof *c);
    c->name = xstrdup(v->name);
    c->flags = v->flags;
    c->val = v->val ? xstrdup(v->val) : NULL;
    if (v->arr) {
        c->arr = xmalloc((size_t)(v->arrn + 1) * sizeof(char *));
        for (int i = 0; i < v->arrn; i++)
            c->arr[i] = v->arr[i] ? xstrdup(v->arr[i]) : NULL;
        c->arrn = v->arrn;
    }
    return c;
}

static void var_save(const char *name)
{
    for (int i = nvsaves - 1; i >= 0 && vsaves[i].frame == vframe; i--)
        if (strcmp(vsaves[i].name, name) == 0)
            return;         /* already saved in this scope */
    if (nvsaves == capvsaves) {
        capvsaves = capvsaves ? capvsaves * 2 : 16;
        vsaves = xrealloc(vsaves, (size_t)capvsaves * sizeof *vsaves);
    }
    vsave_t *s = &vsaves[nvsaves++];
    s->name = xstrdup(name);
    s->old = var_snapshot(var_lookup(name));
    s->frame = vframe;
}

static int locals_mark(void)
{
    vframe++;
    return nvsaves;
}

static void locals_restore(int mark)
{
    while (nvsaves > mark) {
        vsave_t *s = &vsaves[--nvsaves];
        var_t *cur = var_lookup(s->name);
        bool was_exported = cur && (cur->flags & V_EXPORT);
        if (cur)
            var_remove(cur);
        if (s->old) {
            var_t *v = var_create(s->name, strlen(s->name));
            v->flags = s->old->flags;
            v->val = s->old->val;
            v->arr = s->old->arr;
            v->arrn = s->old->arrn;
            xfree(s->old->name);
            xfree(s->old);
            if (v->flags & V_EXPORT)
                env_dirty = true;
        }
        if (was_exported)
            env_dirty = true;
        var_changed(s->name);
        xfree(s->name);
    }
    vframe--;
}

/* `local name[=value]`. Without a value the variable keeps the value it
 * had outside (dash); with one it is set. */
static void var_local(const char *name, const char *val, int flags,
                      bool has_val)
{
    var_t *v = var_lookup(name);
    if (v && (v->flags & V_READONLY)) {
        sh_warn("%s: readonly variable", name);
        return;
    }
    var_save(name);
    if (has_val)
        var_set(name, val, flags);
    else if (flags)
        var_set(name, NULL, flags);
}

static int tmp_scope_marks[64];
static int ntmp_scopes;

/* `x=1 cmd` for a builtin or function: the assignment lasts for the
 * command and is then undone. Scopes nest (a function whose body runs
 * another such command), hence the stack of marks. */
static void var_tmp_scope_begin(void)
{
    if (ntmp_scopes < 64)
        tmp_scope_marks[ntmp_scopes] = locals_mark();
    ntmp_scopes++;
}

static void var_tmp_scope_end(void)
{
    ntmp_scopes--;
    if (ntmp_scopes < 64)
        locals_restore(tmp_scope_marks[ntmp_scopes]);
}

/* ── listing ───────────────────────────────────────────────────────── */

static void sort_strs(char **v, int n)
{
    /* Shell sort: short, no recursion, fine for a few hundred names. */
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++)
            for (int j = i; j >= gap && strcmp(v[j - gap], v[j]) > 0; j -= gap) {
                char *t = v[j];
                v[j] = v[j - gap];
                v[j - gap] = t;
            }
}

static bool safe_unquoted(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++) {
        char c = *s;
        if (!(is_name_char((u8)c) || strchr("@%+=:,./-", c)))
            return false;
    }
    return true;
}

/* Quote a value so the shell reads it back as the same string. */
static void sb_quoted(strbuf_t *b, const char *s)
{
    sb_putc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'')
            sb_puts(b, "'\\''");
        else
            sb_putc(b, *s);
    }
    sb_putc(b, '\'');
}

static void sb_quoted_min(strbuf_t *b, const char *s)
{
    if (safe_unquoted(s))
        sb_puts(b, s);
    else
        sb_quoted(b, s);
}

static void print_quoted(out_t *o, const char *s)
{
    strbuf_t b = {0};
    sb_quoted(&b, s);
    outn(o, b.s, b.len);
    sb_free(&b);
}

static char **var_names_sorted(int *count)
{
    int n = 0;
    for (int h = 0; h < VHASH; h++)
        for (var_t *v = vtab[h]; v; v = v->next)
            n++;
    char **names = xmalloc((size_t)(n + 1) * sizeof(char *));
    int i = 0;
    for (int h = 0; h < VHASH; h++)
        for (var_t *v = vtab[h]; v; v = v->next)
            names[i++] = v->name;
    sort_strs(names, i);
    *count = i;
    return names;
}

static void print_array_value(out_t *o, var_t *v)
{
    outc(o, '(');
    bool first = true;
    for (int i = 0; i < v->arrn; i++) {
        if (!v->arr[i])
            continue;
        if (!first)
            outc(o, ' ');
        first = false;
        outf(o, "[%d]=", i);
        print_quoted(o, v->arr[i]);
    }
    outc(o, ')');
}

/* `set` with no arguments. */
static void var_print_all(bool setform)
{
    (void)setform;
    int n;
    char **names = var_names_sorted(&n);
    for (int i = 0; i < n; i++) {
        var_t *v = var_lookup(names[i]);
        if (!v || (v->flags & V_UNSET))
            continue;
        outs(out1, v->name);
        outc(out1, '=');
        if (v->flags & V_ARRAY)
            print_array_value(out1, v);
        else
            print_quoted(out1, var_get(v->name) ? var_get(v->name) : "");
        outc(out1, '\n');
    }
    xfree(names);
}

/* `export -p`, `readonly -p`: every variable with the flag, written as
 * the command that would recreate it. */
static void var_print_flag(int flag, const char *prefix)
{
    int n;
    char **names = var_names_sorted(&n);
    for (int i = 0; i < n; i++) {
        var_t *v = var_lookup(names[i]);
        if (!v || !(v->flags & flag))
            continue;
        outs(out1, prefix);
        outc(out1, ' ');
        outs(out1, v->name);
        if (v->flags & V_ARRAY) {
            outc(out1, '=');
            print_array_value(out1, v);
        } else if (!(v->flags & V_UNSET) && var_get(v->name)) {
            outc(out1, '=');
            print_quoted(out1, var_get(v->name));
        }
        outc(out1, '\n');
    }
    xfree(names);
}

/* ── positional parameters ─────────────────────────────────────────── */

static void pos_free(void)
{
    for (int i = 0; i < posc; i++)
        xfree(posv[i]);
    xfree(posv);
    posv = NULL;
    posc = 0;
}

static void pos_set(char **v, int n)
{
    char **nv = xmalloc((size_t)(n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++)
        nv[i] = xstrdup(v[i]);
    nv[n] = NULL;
    pos_free();
    posv = nv;
    posc = n;
}

static void vars_init_special(void)
{
    static const char *const specials[] = { "RANDOM", "SECONDS", "LINENO" };
    for (unsigned i = 0; i < sizeof specials / sizeof *specials; i++) {
        var_t *v = var_lookup(specials[i]);
        if (!v)
            v = var_create(specials[i], strlen(specials[i]));
        v->flags = (v->flags & ~V_UNSET) | V_SPECIAL;
    }
    u64 seed = 0;
    lp_getrandom(&seed, sizeof seed, 1);
    rand_state = seed ^ (u64)now_ms();
    seconds_base = now_ms();
}
