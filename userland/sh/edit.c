/* edit.c - the interactive line editor, the history and completion.
 * Part of sh.c; see sh.h.
 *
 * ── Why the layout is computed, not assumed ──
 *
 * The old editor scrolled a long line sideways inside one screen row,
 * because it did not know how the terminal would wrap it. That breaks
 * the moment the line holds Korean: a syllable is three bytes and two
 * columns, and when one lands with a single column left on the row the
 * terminal does not split it - it leaves that column blank and starts the
 * syllable on the next row. An editor that counts bytes, or even
 * characters, then puts the cursor one or two cells off, and every
 * backspace after that deletes something other than what is under it.
 *
 * So this editor models the screen: for every character it works out the
 * row and column the terminal will put it in, applying the terminal's own
 * rule for wide characters at the right edge, and every redraw moves the
 * cursor up to the first row of the line, clears to the end of the
 * screen, writes prompt and line again and puts the cursor back from the
 * computed position. The line may wrap over as many rows as it likes. A
 * redraw is skipped while more typed or pasted bytes are already waiting,
 * so a long paste costs one redraw rather than one per character.
 *
 * Input is read a byte at a time on purpose. A line pasted together with
 * the lines after it must leave those in the terminal for whatever runs
 * next - `cat` pasted with its input, for instance - and a read of 256
 * bytes would swallow them.
 *
 * ── History ──
 *
 * Every line is appended to $HISTFILE (default ~/.lpsh_history) the
 * moment it is entered, so a crash or a power cut loses nothing that was
 * typed before it; trimming the file to $HISTFILESIZE rewrites it through
 * a temporary file, fsync and rename, so an interrupted trim leaves the
 * old file rather than half of one. The old shell's ~/.sh_history is
 * read once if the new file does not exist yet. */

volatile int got_sigwinch;
static int term_cols = 80;
static int term_rows = 24;

static void term_update_size(void)
{
    int r, c;
    if (lp_term_size(tty_fd >= 0 ? tty_fd : 1, &r, &c) == 0 ||
        lp_term_size(0, &r, &c) == 0) {
        term_cols = c;
        term_rows = r;
    }
    if (term_cols < 2)
        term_cols = 80;
    char nb[24];
    var_t *v = var_lookup("COLUMNS");
    if (!v || !(v->flags & V_READONLY))
        var_set("COLUMNS", itoa_s(term_cols, nb), 0);
    v = var_lookup("LINES");
    if (!v || !(v->flags & V_READONLY))
        var_set("LINES", itoa_s(term_rows, nb), 0);
}

static void on_sigwinch(int sig)
{
    (void)sig;
    got_sigwinch = 1;
}

/* ── history ───────────────────────────────────────────────────────── */

static strvec_t hist;
static int      hist_base = 1;      /* the number of hist.v[0] */
static int      hist_loaded_n;      /* entries that came from the file */
static char    *hist_file;

static int hist_limit(const char *name, int dflt)
{
    const char *s = var_get(name);
    long long n;
    if (s && parse_int(s, &n) && n >= 0 && n < 10000000)
        return (int)n;
    return dflt;
}

static const char *histfile_path(void)
{
    const char *hf = var_get("HISTFILE");
    if (hf)
        return *hf ? hf : NULL;
    return hist_file;
}

static void hist_push(const char *line)
{
    int max = hist_limit("HISTSIZE", 1000);
    if (max <= 0)
        return;
    sv_push(&hist, xstrdup(line));
    if (hist.n > max) {
        int drop = hist.n - max;
        for (int i = 0; i < drop; i++)
            xfree(hist.v[i]);
        memmove(hist.v, hist.v + drop, (size_t)(hist.n - drop + 1) * sizeof(char *));
        hist.n -= drop;
        hist_base += drop;
        hist_loaded_n = hist_loaded_n > drop ? hist_loaded_n - drop : 0;
    }
}

static void hist_load_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    strbuf_t all = {0};
    char buf[8192];
    long n;
    while ((n = xread((int)fd, buf, sizeof buf)) > 0)
        sb_putn(&all, buf, (size_t)n);
    lp_close((int)fd);
    const char *p = sb_str(&all);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        /* bash's timestamp lines (#1700000000) are not commands */
        if (l && !(p[0] == '#' && l > 1 && is_digit((u8)p[1]))) {
            char *line = xstrndup(p, l);
            hist_push(line);
            xfree(line);
        }
        p += l + (e ? 1 : 0);
    }
    sb_free(&all);
    hist_loaded_n = hist.n;
}

static void history_init(void)
{
    const char *home = var_get("HOME");
    if (home && *home) {
        strbuf_t b = {0};
        sb_puts(&b, home);
        if (b.s[b.len - 1] != '/')
            sb_putc(&b, '/');
        sb_puts(&b, ".lpsh_history");
        hist_file = sb_take(&b);
    }
    const char *hf = histfile_path();
    if (!hf)
        return;
    if (lp_exists(hf)) {
        hist_load_file(hf);
    } else if (home && *home) {
        /* the old shell's file, once */
        strbuf_t b = {0};
        sb_puts(&b, home);
        sb_puts(&b, "/.sh_history");
        if (lp_exists(b.s))
            hist_load_file(b.s);
        sb_free(&b);
    }
}

static void hist_append_file(const char *line)
{
    const char *hf = histfile_path();
    if (!hf || hist_limit("HISTFILESIZE", 2000) == 0)
        return;
    long fd = lp_open(hf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    strbuf_t b = {0};
    sb_puts(&b, line);
    sb_putc(&b, '\n');
    xwrite_all((int)fd, b.s, b.len);
    sb_free(&b);
    lp_close((int)fd);
}

static void history_add(const char *line)
{
    if (!*line)
        return;
    const char *hc = var_get("HISTCONTROL");
    bool ignspace = true, igndups = true, erasedups = false;
    if (hc) {
        ignspace = strstr(hc, "ignorespace") || strstr(hc, "ignoreboth");
        igndups = strstr(hc, "ignoredups") || strstr(hc, "ignoreboth");
        erasedups = strstr(hc, "erasedups") != NULL;
    }
    if (ignspace && (line[0] == ' ' || line[0] == '\t'))
        return;
    if (igndups && hist.n && strcmp(hist.v[hist.n - 1], line) == 0)
        return;
    if (erasedups) {
        for (int i = hist.n - 1; i >= 0; i--) {
            if (strcmp(hist.v[i], line) == 0) {
                xfree(hist.v[i]);
                memmove(hist.v + i, hist.v + i + 1,
                        (size_t)(hist.n - i) * sizeof(char *));
                hist.n--;
                if (i < hist_loaded_n) hist_loaded_n--;
            }
        }
    }
    hist_push(line);
    hist_append_file(line);
}

static int history_count(void) { return hist.n; }

static const char *history_get(int idx)
{
    return idx >= 0 && idx < hist.n ? hist.v[idx] : "";
}

static int history_number(int idx) { return hist_base + idx; }

static void history_replace_last(const char *s)
{
    if (hist.n) {
        xfree(hist.v[hist.n - 1]);
        hist.v[hist.n - 1] = xstrdup(s);
    }
}

/* fc's way of naming an entry: a number, a negative offset, or the most
 * recent entry starting with the text. last is the first index not to
 * consider (the fc command itself). */
static int history_find(const char *spec, int last)
{
    long long n;
    if (parse_int(spec, &n)) {
        int idx = n < 0 ? last + (int)n : (int)n - hist_base;
        return idx >= 0 && idx < last ? idx : -1;
    }
    size_t l = strlen(spec);
    for (int i = last - 1; i >= 0; i--)
        if (strncmp(hist.v[i], spec, l) == 0)
            return i;
    return -1;
}

static void history_list(int n)
{
    int start = n < 0 || n > hist.n ? 0 : hist.n - n;
    for (int i = start; i < hist.n; i++)
        outf(out1, "%5d  %s\n", hist_base + i, hist.v[i]);
}

static void history_clear(void)
{
    sv_free(&hist);
    hist_loaded_n = 0;
}

static bool history_delete(int n)
{
    int idx = n < 0 ? hist.n + n : n - hist_base;
    if (idx < 0 || idx >= hist.n)
        return false;
    xfree(hist.v[idx]);
    memmove(hist.v + idx, hist.v + idx + 1, (size_t)(hist.n - idx) * sizeof(char *));
    hist.n--;
    return true;
}

/* Write the file as a whole: temporary file, fsync, rename, fsync the
 * directory. Used to trim it and for `history -w`. */
static void hist_rewrite(const char *path, char **lines, int n)
{
    strbuf_t tmp = {0};
    sb_puts(&tmp, path);
    sb_printf(&tmp, ".tmp%d", lp_getpid());
    long fd = lp_open(tmp.s, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        sb_free(&tmp);
        return;
    }
    strbuf_t all = {0};
    for (int i = 0; i < n; i++) {
        sb_puts(&all, lines[i]);
        sb_putc(&all, '\n');
    }
    bool ok = xwrite_all((int)fd, all.s ? all.s : "", all.len) &&
              lp_fsync((int)fd) == 0;
    lp_close((int)fd);
    sb_free(&all);
    if (ok && lp_rename(tmp.s, path) == 0) {
        strbuf_t dir = {0};
        const char *slash = strrchr(path, '/');
        if (slash && slash != path)
            sb_putn(&dir, path, (size_t)(slash - path));
        else
            sb_puts(&dir, slash ? "/" : ".");
        long dfd = lp_open(dir.s, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        if (dfd >= 0) {
            lp_fsync((int)dfd);
            lp_close((int)dfd);
        }
        sb_free(&dir);
    } else {
        lp_unlink(tmp.s);
    }
    sb_free(&tmp);
}

/* At exit and on `history -w`: keep the file within HISTFILESIZE. Lines
 * were appended as they were typed, so this only ever shortens it - and
 * leaves alone a file other shells are appending to unless it has grown
 * well past the limit. */
static bool hist_force_write;

static void history_write(void)
{
    const char *hf = histfile_path();
    if (!hf)
        return;
    int max = hist_limit("HISTFILESIZE", 2000);
    if (hist_force_write) {
        hist_force_write = false;
        int start = hist.n > max ? hist.n - max : 0;
        hist_rewrite(hf, hist.v + start, hist.n - start);
        return;
    }
    long fd = lp_open(hf, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    strbuf_t all = {0};
    char buf[8192];
    long n;
    while ((n = xread((int)fd, buf, sizeof buf)) > 0)
        sb_putn(&all, buf, (size_t)n);
    lp_close((int)fd);
    int lines = 0;
    for (size_t i = 0; i < all.len; i++)
        if (all.s[i] == '\n') lines++;
    if (lines > max + max / 4 + 16) {
        strvec_t keep = {0};
        const char *p = sb_str(&all);
        int skip = lines - max;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (skip > 0)
                skip--;
            else
                sv_push(&keep, xstrndup(p, l));
            p += l + (e ? 1 : 0);
        }
        hist_rewrite(hf, keep.v, keep.n);
        sv_free(&keep);
    }
    sb_free(&all);
}

/* ── history expansion: !! !$ !n !str ^a^b ─────────────────────────── */

/* Split a history line into words the way the shell would, roughly:
 * blanks separate, quotes group, and the operators ; | & < > ( ) are
 * words of their own. Good enough for !$ and !*. */
static void hist_words(const char *s, strvec_t *out)
{
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *st = p;
        if (strchr(";|&<>()", *p)) {
            char c = *p++;
            if ((c == '|' || c == '&' || c == '>' || c == '<') && *p == c)
                p++;
            sv_push(out, xstrndup(st, (size_t)(p - st)));
            continue;
        }
        while (*p && *p != ' ' && *p != '\t' && !strchr(";|&<>()", *p)) {
            if (*p == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                while (*p && *p != q) {
                    if (q == '"' && *p == '\\' && p[1]) p++;
                    p++;
                }
                if (*p) p++;
                continue;
            }
            p++;
        }
        sv_push(out, xstrndup(st, (size_t)(p - st)));
    }
}

static bool hist_modify(strbuf_t *val, const char **pp, bool *print,
                        char **last_old, char **last_new)
{
    const char *p = *pp;
    while (*p == ':') {
        char m = p[1];
        bool global = false;
        if (m == 'g' || m == 'a') {
            global = true;
            m = p[2];
            p++;
        }
        switch (m) {
        case 'h': {
            char *s = sb_str(val);
            char *sl = strrchr(s, '/');
            if (sl) { *sl = '\0'; val->len = (size_t)(sl - s); }
            p += 2;
            break;
        }
        case 't': {
            char *s = sb_str(val);
            char *sl = strrchr(s, '/');
            if (sl) {
                char *t = xstrdup(sl + 1);
                val->len = 0;
                sb_puts(val, t);
                xfree(t);
            }
            p += 2;
            break;
        }
        case 'r': {
            char *s = sb_str(val);
            char *dot = strrchr(s, '.');
            char *sl = strrchr(s, '/');
            if (dot && (!sl || dot > sl)) { *dot = '\0'; val->len = (size_t)(dot - s); }
            p += 2;
            break;
        }
        case 'e': {
            char *s = sb_str(val);
            char *dot = strrchr(s, '.');
            char *sl = strrchr(s, '/');
            char *t = xstrdup(dot && (!sl || dot > sl) ? dot + 1 : "");
            val->len = 0;
            sb_puts(val, t);
            xfree(t);
            p += 2;
            break;
        }
        case 'p':
            *print = true;
            p += 2;
            break;
        case 'q': {
            char *t = xstrdup(sb_str(val));
            val->len = 0;
            sb_quoted(val, t);
            xfree(t);
            p += 2;
            break;
        }
        case '&':
        case 's': {
            if (m == 's') {
                char delim = p[2];
                if (!delim) return false;
                const char *a = p + 3;
                const char *b = strchr(a, delim);
                if (!b) return false;
                const char *c = strchr(b + 1, delim);
                size_t cl = c ? (size_t)(c - (b + 1)) : strlen(b + 1);
                xfree(*last_old);
                xfree(*last_new);
                *last_old = xstrndup(a, (size_t)(b - a));
                *last_new = xstrndup(b + 1, cl);
                p = c ? c + 1 : b + 1 + cl;
            } else {
                p += 2;
            }
            if (!*last_old || !**last_old)
                return false;
            char *s = xstrdup(sb_str(val));
            val->len = 0;
            const char *q = s;
            bool done = false;
            while (*q) {
                const char *hit = done ? NULL : strstr(q, *last_old);
                if (!hit) {
                    sb_puts(val, q);
                    break;
                }
                sb_putn(val, q, (size_t)(hit - q));
                for (const char *r = *last_new; *r; r++) {
                    if (*r == '&') sb_puts(val, *last_old);
                    else if (*r == '\\' && r[1]) sb_putc(val, *++r);
                    else sb_putc(val, *r);
                }
                q = hit + strlen(*last_old);
                if (!global) done = true;
            }
            xfree(s);
            break;
        }
        default:
            *pp = p;
            return true;
        }
    }
    *pp = p;
    return true;
}

static bool history_expand(const char *line, strbuf_t *out, bool *print)
{
    *print = false;
    bool changed = false;
    char *last_old = NULL, *last_new = NULL;
    const char *p = line;
    /* ^old^new^ at the very start */
    if (line[0] == '^' && hist.n) {
        const char *a = line + 1;
        const char *b = strchr(a, '^');
        if (b) {
            const char *c = strchr(b + 1, '^');
            size_t nl = c ? (size_t)(c - (b + 1)) : strlen(b + 1);
            char *old = xstrndup(a, (size_t)(b - a));
            char *new = xstrndup(b + 1, nl);
            const char *prev = hist.v[hist.n - 1];
            const char *hit = *old ? strstr(prev, old) : NULL;
            if (!hit) {
                sh_warn("%s: substitution failed", old);
                xfree(old);
                xfree(new);
                return false;
            }
            sb_putn(out, prev, (size_t)(hit - prev));
            sb_puts(out, new);
            sb_puts(out, hit + strlen(old));
            if (c) sb_puts(out, c + 1);
            xfree(old);
            xfree(new);
            *print = true;
            return true;
        }
    }
    bool sq = false, dq = false;
    while (*p) {
        char c = *p;
        if (c == '\\' && p[1] && !sq) {
            sb_putc(out, c);
            sb_putc(out, p[1]);
            p += 2;
            continue;
        }
        if (c == '\'' && !dq) sq = !sq;
        else if (c == '"') dq = !dq;
        if (c != '!' || sq || !p[1] || strchr(" \t\n=(", p[1]) ||
            (dq && p[1] == '"')) {
            sb_putc(out, c);
            p++;
            continue;
        }
        /* an event */
        const char *q = p + 1;
        int idx = -1;
        if (*q == '!') {
            idx = hist.n - 1;
            q++;
        } else if (*q == '$' || *q == '^' || *q == '*' || *q == ':') {
            idx = hist.n - 1;           /* !$ is !!:$ */
        } else if (is_digit((u8)*q) || (*q == '-' && is_digit((u8)q[1]))) {
            bool neg = *q == '-';
            if (neg) q++;
            long n = 0;
            while (is_digit((u8)*q)) n = n * 10 + (*q++ - '0');
            idx = neg ? hist.n - (int)n : (int)n - hist_base;
        } else if (*q == '#') {
            /* the line so far */
            sb_puts(out, "");
            char *sofar = xstrndup(out->s ? out->s : "", out->len);
            sb_puts(out, sofar);
            xfree(sofar);
            p = q + 1;
            changed = true;
            continue;
        } else {
            bool contains = *q == '?';
            if (contains) q++;
            const char *st = q;
            while (*q && !strchr(contains ? "?\n" : " \t\n:;|&<>()\"'", *q))
                q++;
            char *pat = xstrndup(st, (size_t)(q - st));
            if (contains && *q == '?') q++;
            for (int i = hist.n - 1; i >= 0; i--) {
                if (contains ? strstr(hist.v[i], pat) != NULL
                             : strncmp(hist.v[i], pat, strlen(pat)) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                sh_warn("!%s%s: event not found", contains ? "?" : "", pat);
                xfree(pat);
                xfree(last_old);
                xfree(last_new);
                return false;
            }
            xfree(pat);
        }
        if (idx < 0 || idx >= hist.n) {
            sh_warn("%.*s: event not found", (int)(q - p), p);
            xfree(last_old);
            xfree(last_new);
            return false;
        }
        const char *ev = hist.v[idx];
        strbuf_t val = {0};
        /* word designators */
        bool have_wd = false;
        int w0 = 0, w1 = -1;        /* -1: to the end */
        const char *d = q;
        if (*d == ':' && d[1] && strchr("0123456789^$*-", d[1])) {
            d++;
            have_wd = true;
        } else if (*d == '$' || *d == '^' || *d == '*') {
            have_wd = true;
        }
        if (have_wd) {
            strvec_t words = {0};
            hist_words(ev, &words);
            int nw = words.n;
            if (*d == '$') { w0 = w1 = nw - 1; d++; }
            else if (*d == '^') { w0 = w1 = 1; d++; }
            else if (*d == '*') { w0 = 1; w1 = nw - 1; d++; }
            else {
                if (*d == '-') {
                    w0 = 0;
                } else {
                    w0 = 0;
                    while (is_digit((u8)*d)) w0 = w0 * 10 + (*d++ - '0');
                }
                w1 = w0;
                if (*d == '-') {
                    d++;
                    if (*d == '$') { w1 = nw - 1; d++; }
                    else if (is_digit((u8)*d)) {
                        w1 = 0;
                        while (is_digit((u8)*d)) w1 = w1 * 10 + (*d++ - '0');
                    } else {
                        w1 = nw - 2;
                    }
                } else if (*d == '*') {
                    w1 = nw - 1;
                    d++;
                }
            }
            if (w0 < 0 || w0 >= nw || w1 >= nw || (w1 < w0 && !(w0 == 1 && w1 == 0))) {
                if (!(w0 == 1 && nw == 1 && w1 == 0)) {
                    sv_free(&words);
                    sh_warn("%.*s: bad word specifier", (int)(d - p), p);
                    xfree(last_old);
                    xfree(last_new);
                    sb_free(&val);
                    return false;
                }
            }
            for (int k = w0; k <= w1 && k < nw; k++) {
                if (k > w0) sb_putc(&val, ' ');
                sb_puts(&val, words.v[k]);
            }
            sv_free(&words);
            q = d;
        } else {
            sb_puts(&val, ev);
        }
        if (!hist_modify(&val, &q, print, &last_old, &last_new)) {
            sh_warn("history: bad modifier");
            sb_free(&val);
            xfree(last_old);
            xfree(last_new);
            return false;
        }
        sb_putn(out, val.s ? val.s : "", val.len);
        sb_free(&val);
        p = q;
        changed = true;
    }
    sb_str(out);
    xfree(last_old);
    xfree(last_new);
    if (changed)
        *print = true;
    return true;
}

/* ── the terminal ──────────────────────────────────────────────────── */

typedef struct {
    strbuf_t     buf;
    size_t       cur;
    char        *prompt_all;    /* the whole prompt, as written */
    char        *prompt_last;   /* its last line, for redraws */
    int          pw;            /* display width of the last line */
    int          cols;
    int          cur_row;       /* where the cursor was left, from row 0 */
    int          ifd, ofd;
    strbuf_t     out;           /* what the next flush writes */
    /* history browsing */
    int          hpos;          /* hist index shown, hist.n = the new line */
    char        *saved;         /* the new line while browsing */
    /* kill ring (one entry) and undo */
    char        *killed;
    strvec_t     undo;
    size_t      *undo_cur;
    int          undo_cap;
    bool         last_was_kill;
    bool         last_was_tab;
    int          lastarg_n;     /* Alt-. repeat count */
    size_t       lastarg_at, lastarg_len;
    bool         last_was_lastarg;
} ed_t;

static void ed_flush(ed_t *e)
{
    if (e->out.len)
        xwrite_all(e->ofd, e->out.s, e->out.len);
    e->out.len = 0;
}

static bool input_pending(int fd)
{
    int n = 0;
    return lp_ioctl(fd, 0x541B /* FIONREAD */, &n) == 0 && n > 0;
}

/* One character's appearance: the bytes to write and its width. Control
 * characters show as ^X; bytes that are not UTF-8 as '?'. */
static int char_look(const char *s, size_t len, size_t i, size_t *used,
                     char *look, size_t *looklen)
{
    u8 c = (u8)s[i];
    if (c < 0x20 || c == 0x7f) {
        *used = 1;
        look[0] = '^';
        look[1] = c == 0x7f ? '?' : (char)(c + '@');
        *looklen = 2;
        return 2;
    }
    if (c < 0x80) {
        *used = 1;
        look[0] = (char)c;
        *looklen = 1;
        return 1;
    }
    int n;
    u32 cp = utf8_decode(s + i, len - i, &n);
    if (n <= 0) n = 1;
    *used = (size_t)n;
    if (cp == 0xFFFD && !(n == 3 && (u8)s[i] == 0xEF)) {
        look[0] = '?';
        *looklen = 1;
        return 1;
    }
    memcpy(look, s + i, (size_t)n);
    *looklen = (size_t)n;
    return utf8_width(cp);
}

/* Advance (row, col) over a cell of width w with the terminal's rules. */
static void layout_step(int *row, int *col, int w, int cols)
{
    if (w == 0)
        return;
    if (*col + w > cols) {
        (*row)++;
        *col = 0;
    }
    *col += w;
}

/* Where the cursor sits after the text up to byte index `upto`. A
 * position exactly at the right edge is shown at the start of the next
 * row, which is where the next character would go. */
static void layout_pos(ed_t *e, size_t upto, int *row, int *col)
{
    int r = 0, c = e->pw;
    while (c > e->cols) {
        c -= e->cols;
        r++;
    }
    const char *s = e->buf.s ? e->buf.s : "";
    for (size_t i = 0; i < upto && i < e->buf.len; ) {
        char look[8];
        size_t used, ll;
        int w = char_look(s, e->buf.len, i, &used, look, &ll);
        if (look[0] == '^' && ll == 2 && w == 2) {
            layout_step(&r, &c, 1, e->cols);
            layout_step(&r, &c, 1, e->cols);
        } else {
            layout_step(&r, &c, w, e->cols);
        }
        i += used;
    }
    if (c >= e->cols) {
        r++;
        c = 0;
    }
    *row = r;
    *col = c;
}

static void ed_refresh(ed_t *e)
{
    strbuf_t *o = &e->out;
    if (e->cur_row > 0)
        sb_printf(o, "\x1b[%dA", e->cur_row);
    sb_puts(o, "\r\x1b[J");
    sb_puts(o, e->prompt_last);
    const char *s = e->buf.s ? e->buf.s : "";
    for (size_t i = 0; i < e->buf.len; ) {
        char look[8];
        size_t used, ll;
        char_look(s, e->buf.len, i, &used, look, &ll);
        sb_putn(o, look, ll);
        i += used;
    }
    int er, ec, cr, cc;
    layout_pos(e, e->buf.len, &er, &ec);
    /* layout_pos already moved an edge position to the next row; make
     * the terminal agree by giving it that row */
    {
        int rr = 0, c2 = e->pw;
        while (c2 > e->cols) { c2 -= e->cols; rr++; }
        const char *t = s;
        for (size_t i = 0; i < e->buf.len; ) {
            char look[8];
            size_t used, ll;
            int w = char_look(t, e->buf.len, i, &used, look, &ll);
            if (look[0] == '^' && ll == 2 && w == 2) {
                layout_step(&rr, &c2, 1, e->cols);
                layout_step(&rr, &c2, 1, e->cols);
            } else {
                layout_step(&rr, &c2, w, e->cols);
            }
            i += used;
        }
        if (c2 >= e->cols)
            sb_puts(o, "\r\n");
    }
    layout_pos(e, e->cur, &cr, &cc);
    if (er > cr)
        sb_printf(o, "\x1b[%dA", er - cr);
    sb_putc(o, '\r');
    if (cc > 0)
        sb_printf(o, "\x1b[%dC", cc);
    e->cur_row = cr;
    ed_flush(e);
}

/* Move the cursor to after the last row, for Enter and ^C. */
static void ed_finish_line(ed_t *e)
{
    int er, ec, cr, cc;
    layout_pos(e, e->buf.len, &er, &ec);
    layout_pos(e, e->cur, &cr, &cc);
    if (er > cr)
        sb_printf(&e->out, "\x1b[%dB", er - cr);
    (void)ec;
    (void)cc;
    sb_puts(&e->out, "\r\n");
    ed_flush(e);
}

/* ── editing primitives ────────────────────────────────────────────── */

static bool is_zero_width_at(const char *s, size_t len, size_t i)
{
    if (i >= len || (u8)s[i] < 0x80)
        return false;
    int n;
    u32 cp = utf8_decode(s + i, len - i, &n);
    return utf8_width(cp) == 0 && cp != 0xFFFD;
}

static size_t next_char(ed_t *e, size_t i)
{
    const char *s = e->buf.s;
    size_t len = e->buf.len;
    if (i >= len)
        return len;
    i = utf8_next(s, len, i);
    while (i < len && is_zero_width_at(s, len, i))
        i = utf8_next(s, len, i);
    return i;
}

static size_t prev_char(ed_t *e, size_t i)
{
    const char *s = e->buf.s;
    if (i == 0)
        return 0;
    i = utf8_prev(s, i);
    while (i > 0 && is_zero_width_at(s, e->buf.len, i))
        i = utf8_prev(s, i);
    return i;
}

static bool is_word_byte(u8 c)
{
    return c >= 0x80 || is_name_char(c);
}

static size_t word_left(ed_t *e, size_t i)
{
    const char *s = e->buf.s;
    while (i > 0 && !is_word_byte((u8)s[i - 1]))
        i--;
    while (i > 0 && is_word_byte((u8)s[i - 1]))
        i--;
    while (i > 0 && ((u8)s[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static size_t word_right(ed_t *e, size_t i)
{
    const char *s = e->buf.s;
    size_t len = e->buf.len;
    while (i < len && !is_word_byte((u8)s[i]))
        i++;
    while (i < len && is_word_byte((u8)s[i]))
        i++;
    return i;
}

static void ed_undo_push(ed_t *e)
{
    if (e->undo.n && strcmp(e->undo.v[e->undo.n - 1], e->buf.s ? e->buf.s : "") == 0)
        return;
    if (e->undo.n >= 200) {
        xfree(e->undo.v[0]);
        memmove(e->undo.v, e->undo.v + 1, (size_t)e->undo.n * sizeof(char *));
        memmove(e->undo_cur, e->undo_cur + 1, (size_t)(e->undo.n - 1) * sizeof(size_t));
        e->undo.n--;
    }
    sv_push(&e->undo, xstrdup(e->buf.s ? e->buf.s : ""));
    if (e->undo.n > e->undo_cap) {
        e->undo_cap = e->undo.n * 2;
        e->undo_cur = xrealloc(e->undo_cur, (size_t)e->undo_cap * sizeof(size_t));
    }
    e->undo_cur[e->undo.n - 1] = e->cur;
}

static void ed_set(ed_t *e, const char *s)
{
    e->buf.len = 0;
    sb_puts(&e->buf, s);
    sb_str(&e->buf);
    e->cur = e->buf.len;
}

static void ed_insert(ed_t *e, const char *s, size_t n)
{
    sb_grow(&e->buf, n);
    memmove(e->buf.s + e->cur + n, e->buf.s + e->cur, e->buf.len - e->cur);
    memcpy(e->buf.s + e->cur, s, n);
    e->buf.len += n;
    e->buf.s[e->buf.len] = '\0';
    e->cur += n;
}

static void ed_delete(ed_t *e, size_t from, size_t to, bool kill)
{
    if (from >= to)
        return;
    if (kill) {
        if (e->last_was_kill && e->killed) {
            /* consecutive kills accumulate, as in readline */
            strbuf_t b = {0};
            if (from < e->cur || to <= e->cur) {
                sb_putn(&b, e->buf.s + from, to - from);
                sb_puts(&b, e->killed);
            } else {
                sb_puts(&b, e->killed);
                sb_putn(&b, e->buf.s + from, to - from);
            }
            xfree(e->killed);
            e->killed = sb_take(&b);
        } else {
            xfree(e->killed);
            e->killed = xstrndup(e->buf.s + from, to - from);
        }
    }
    memmove(e->buf.s + from, e->buf.s + to, e->buf.len - to);
    e->buf.len -= to - from;
    e->buf.s[e->buf.len] = '\0';
    e->cur = from;
}

/* ── reading keys ──────────────────────────────────────────────────── */

enum {
    K_NONE = 0x110000, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
    K_INS, K_PGUP, K_PGDN, K_CLEFT, K_CRIGHT, K_CDEL, K_EOF, K_INTR,
    K_WINCH, K_ALT = 0x200000
};

static int read_byte(int fd, long timeout_ms)
{
    for (;;) {
        if (timeout_ms >= 0) {
            int w = fd_wait(fd, timeout_ms);
            if (w == 0)
                return -3;          /* timeout */
            if (w == -2) {
                if (got_sigwinch) return -4;
                if (pending_signals) return -5;
                continue;
            }
        }
        u8 c;
        long n = lp_read(fd, &c, 1);
        if (n == 1)
            return c;
        if (n == -E_INTR) {
            if (got_sigwinch)
                return -4;
            if (pending_signals && (gotsig[1] || got_sigint))
                return -5;
            continue;
        }
        return -1;
    }
}

/* One key: a Unicode character (its UTF-8 bytes go in seq), or one of
 * the K_ codes. Alt+x arrives as ESC x and comes back as K_ALT | x. */
static int read_key(int fd, char *seq, int *seqlen)
{
    *seqlen = 0;
    int c = read_byte(fd, -1);
    if (c == -1)
        return K_EOF;
    if (c == -4)
        return K_WINCH;
    if (c == -5)
        return K_INTR;
    if (c == 27) {
        int d = read_byte(fd, 50);
        if (d < 0)
            return 27;
        if (d == '[' || d == 'O') {
            char params[16];
            int np = 0;
            int f;
            for (;;) {
                f = read_byte(fd, 50);
                if (f < 0)
                    return K_NONE;
                if ((f >= '0' && f <= '9') || f == ';') {
                    if (np < 15) params[np++] = (char)f;
                    continue;
                }
                break;
            }
            params[np] = '\0';
            bool ctrl = strstr(params, ";5") != NULL;
            bool alt = strstr(params, ";3") != NULL;
            switch (f) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return ctrl || alt ? K_CRIGHT : K_RIGHT;
            case 'D': return ctrl || alt ? K_CLEFT : K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case '~': {
                int n = atoi(params);
                switch (n) {
                case 1: case 7: return K_HOME;
                case 4: case 8: return K_END;
                case 3: return ctrl ? K_CDEL : K_DEL;
                case 2: return K_INS;
                case 5: return K_PGUP;
                case 6: return K_PGDN;
                case 200: case 201: return K_NONE;
                }
                return K_NONE;
            }
            }
            return K_NONE;
        }
        if (d == 127 || d == 8)
            return K_ALT | 127;
        return K_ALT | d;
    }
    seq[0] = (char)c;
    *seqlen = 1;
    if (c >= 0x80) {
        int need = utf8_seq_len((u8)c);
        for (int i = 1; i < need; i++) {
            int d = read_byte(fd, 100);
            if (d < 0 || ((u8)d & 0xC0) != 0x80) {
                break;
            }
            seq[(*seqlen)++] = (char)d;
        }
        int n;
        u32 cp = utf8_decode(seq, (size_t)*seqlen, &n);
        return (int)cp;
    }
    return c;
}

/* ── completion ────────────────────────────────────────────────────── */

typedef struct {
    strvec_t names;         /* as shown in a listing */
    strvec_t inserts;       /* what goes in the line (quoted) */
    bool     *isdir;
    int       capdir;
} comp_t;

static void comp_add(comp_t *c, const char *show, const char *insert,
                     bool isdir)
{
    for (int i = 0; i < c->names.n; i++)
        if (strcmp(c->names.v[i], show) == 0)
            return;
    sv_push(&c->names, xstrdup(show));
    sv_push(&c->inserts, xstrdup(insert));
    if (c->names.n > c->capdir) {
        c->capdir = c->names.n * 2 + 8;
        c->isdir = xrealloc(c->isdir, (size_t)c->capdir * sizeof(bool));
    }
    c->isdir[c->names.n - 1] = isdir;
}

static void comp_free(comp_t *c)
{
    sv_free(&c->names);
    sv_free(&c->inserts);
    xfree(c->isdir);
    memset(c, 0, sizeof *c);
}

/* Backslash-quote what the shell would otherwise take apart. */
static void comp_quote(strbuf_t *b, const char *s)
{
    for (; *s; s++) {
        if (strchr(" \t\n\\'\"`$&|;<>()[]*?!#{}~=", *s))
            sb_putc(b, '\\');
        sb_putc(b, *s);
    }
}

/* The typed word with its quoting removed. */
static char *comp_unquote(const char *s, size_t n)
{
    strbuf_t b = {0};
    char q = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (q) {
            if (c == q) { q = 0; continue; }
            if (q == '"' && c == '\\' && i + 1 < n) c = s[++i];
            sb_putc(&b, c);
            continue;
        }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == '\\' && i + 1 < n) { sb_putc(&b, s[++i]); continue; }
        sb_putc(&b, c);
    }
    return sb_take(&b);
}

static void comp_files(comp_t *c, const char *word, bool dirs_only,
                       bool exec_only)
{
    const char *slash = strrchr(word, '/');
    strbuf_t dir = {0}, shown_dir = {0};
    const char *prefix = word;
    if (slash) {
        sb_putn(&shown_dir, word, (size_t)(slash - word + 1));
        prefix = slash + 1;
        /* ~ and ~user at the front */
        if (word[0] == '~') {
            const char *e = strchr(word, '/');
            char *user = xstrndup(word + 1, (size_t)(e - word - 1));
            char *home = tilde_home(user);
            xfree(user);
            if (home) {
                sb_puts(&dir, home);
                sb_putn(&dir, e, (size_t)(slash - e + 1));
                xfree(home);
            } else {
                sb_putn(&dir, word, (size_t)(slash - word + 1));
            }
        } else {
            sb_putn(&dir, word, (size_t)(slash - word + 1));
        }
    }
    const char *opendir = dir.len ? dir.s : ".";
    long fd = lp_open(opendir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0) {
        sb_free(&dir);
        sb_free(&shown_dir);
        return;
    }
    size_t pl = strlen(prefix);
    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char *rec = buf + off;
            u16 reclen;
            memcpy(&reclen, rec + 16, sizeof reclen);
            u8 dtype = (u8)rec[18];
            const char *name = rec + 19;
            off += reclen;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (name[0] == '.' && prefix[0] != '.')
                continue;
            if (strncmp(name, prefix, pl) != 0)
                continue;
            bool isdir = dtype == 4;
            if (dtype == 10 || dtype == 0) {
                strbuf_t full = {0};
                sb_puts(&full, dir.len ? dir.s : "");
                sb_puts(&full, name);
                isdir = lp_is_dir(full.s);
                if (exec_only && !isdir && !is_exec_file(full.s)) {
                    sb_free(&full);
                    continue;
                }
                sb_free(&full);
            } else if (exec_only && !isdir) {
                strbuf_t full = {0};
                sb_puts(&full, dir.len ? dir.s : "");
                sb_puts(&full, name);
                bool x = is_exec_file(full.s);
                sb_free(&full);
                if (!x)
                    continue;
            }
            if (dirs_only && !isdir)
                continue;
            strbuf_t ins = {0};
            sb_puts(&ins, shown_dir.len ? shown_dir.s : "");
            if (word[0] == '~' && slash) {
                /* keep the ~ unquoted */
                ins.len = 0;
                sb_putn(&ins, word, (size_t)(slash - word + 1));
            } else if (shown_dir.len) {
                ins.len = 0;
                comp_quote(&ins, shown_dir.s);
            }
            comp_quote(&ins, name);
            comp_add(c, name, sb_str(&ins), isdir);
            sb_free(&ins);
        }
    }
    lp_close((int)fd);
    sb_free(&dir);
    sb_free(&shown_dir);
}

static void comp_commands(comp_t *c, const char *word)
{
    size_t wl = strlen(word);
    for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++)
        if (strncmp(keywords[i].w, word, wl) == 0 && keywords[i].w[0] != '{' &&
            keywords[i].w[0] != '}')
            comp_add(c, keywords[i].w, keywords[i].w, false);
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (strncmp(builtins[i].name, word, wl) == 0)
            comp_add(c, builtins[i].name, builtins[i].name, false);
    for (int h = 0; h < AHASH; h++)
        for (alias_t *a = atab[h]; a; a = a->next)
            if (strncmp(a->name, word, wl) == 0)
                comp_add(c, a->name, a->name, false);
    for (int h = 0; h < FHASH; h++)
        for (func_t *f = ftab[h]; f; f = f->next)
            if (strncmp(f->name, word, wl) == 0)
                comp_add(c, f->name, f->name, false);
    const char *path = var_get("PATH");
    if (!path)
        return;
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        strbuf_t d = {0};
        if (dl) sb_putn(&d, p, dl); else sb_putc(&d, '.');
        long fd = lp_open(d.s, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        if (fd >= 0) {
            char buf[8192];
            for (;;) {
                long n = sys_getdents((int)fd, buf, sizeof buf);
                if (n <= 0)
                    break;
                for (long off = 0; off < n; ) {
                    char *rec = buf + off;
                    u16 reclen;
                    memcpy(&reclen, rec + 16, sizeof reclen);
                    u8 dtype = (u8)rec[18];
                    const char *name = rec + 19;
                    off += reclen;
                    if (name[0] == '.' || strncmp(name, word, wl) != 0)
                        continue;
                    if (dtype == 4)
                        continue;
                    strbuf_t full = {0};
                    sb_puts(&full, d.s);
                    sb_putc(&full, '/');
                    sb_puts(&full, name);
                    bool x = is_exec_file(full.s);
                    sb_free(&full);
                    if (!x)
                        continue;
                    strbuf_t ins = {0};
                    comp_quote(&ins, name);
                    comp_add(c, name, sb_str(&ins), false);
                    sb_free(&ins);
                }
            }
            lp_close((int)fd);
        }
        sb_free(&d);
        if (!e)
            break;
        p = e + 1;
    }
}

static void comp_vars(comp_t *c, const char *prefix, bool braced)
{
    size_t pl = strlen(prefix);
    int n;
    char **names = var_names_sorted(&n);
    for (int i = 0; i < n; i++) {
        var_t *v = var_lookup(names[i]);
        if (!v || (v->flags & V_UNSET))
            continue;
        if (strncmp(names[i], prefix, pl) != 0)
            continue;
        strbuf_t b = {0};
        sb_puts(&b, braced ? "${" : "$");
        sb_puts(&b, names[i]);
        if (braced) sb_putc(&b, '}');
        comp_add(c, names[i], sb_str(&b), false);
        sb_free(&b);
    }
    xfree(names);
}

static void comp_users(comp_t *c, const char *prefix)
{
    long fd = lp_open("/etc/passwd", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    strbuf_t all = {0};
    char buf[4096];
    long n;
    while ((n = xread((int)fd, buf, sizeof buf)) > 0)
        sb_putn(&all, buf, (size_t)n);
    lp_close((int)fd);
    size_t pl = strlen(prefix);
    const char *p = sb_str(&all);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *colon = memchr(p, ':', l);
        if (colon) {
            char *name = xstrndup(p, (size_t)(colon - p));
            if (strncmp(name, prefix, pl) == 0) {
                strbuf_t ins = {0};
                sb_putc(&ins, '~');
                sb_puts(&ins, name);
                sb_putc(&ins, '/');
                strbuf_t sh = {0};
                sb_putc(&sh, '~');
                sb_puts(&sh, name);
                comp_add(c, sh.s, ins.s, true);
                sb_free(&ins);
                sb_free(&sh);
            }
            xfree(name);
        }
        p += l + (e ? 1 : 0);
    }
    sb_free(&all);
}

/* Where the word being completed starts, and whether it is in command
 * position: first on the line, or after ; | & && || ( or a word like
 * sudo that runs the next one. */
static size_t comp_word_start(ed_t *e, bool *cmdpos)
{
    const char *s = e->buf.s ? e->buf.s : "";
    size_t i = 0, start = 0;
    bool cmd = true;
    char q = 0;
    bool prev_runs_cmd = false;
    size_t word_begin = 0;
    bool in_word = false;
    for (i = 0; i < e->cur; i++) {
        char c = s[i];
        if (q) {
            if (c == q) q = 0;
            else if (q == '"' && c == '\\') i++;
            continue;
        }
        if (c == '\\') { if (!in_word) { in_word = true; word_begin = i; } i++; continue; }
        if (c == '\'' || c == '"') {
            if (!in_word) { in_word = true; word_begin = i; }
            q = c;
            continue;
        }
        if (c == ' ' || c == '\t' || strchr(";|&(<>", c)) {
            if (in_word) {
                /* a word ended: was it one that runs the next word? */
                char *w = xstrndup(s + word_begin, i - word_begin);
                prev_runs_cmd = cmd && (strcmp(w, "sudo") == 0 ||
                    strcmp(w, "command") == 0 || strcmp(w, "exec") == 0 ||
                    strcmp(w, "time") == 0 || strcmp(w, "nohup") == 0 ||
                    strcmp(w, "then") == 0 || strcmp(w, "do") == 0 ||
                    strcmp(w, "else") == 0 || strcmp(w, "if") == 0 ||
                    strcmp(w, "while") == 0 || strcmp(w, "until") == 0 ||
                    strcmp(w, "!") == 0 || strcmp(w, "{") == 0 ||
                    strcmp(w, "nice") == 0 || strcmp(w, "watch") == 0 ||
                    strcmp(w, "builtin") == 0 || strcmp(w, "type") == 0 ||
                    strcmp(w, "which") == 0 || strcmp(w, "man") == 0 ||
                    strcmp(w, "help") == 0);
                bool assign = cmd && strchr(w, '=') && is_name_start((u8)w[0]);
                xfree(w);
                cmd = prev_runs_cmd || assign;
                in_word = false;
            }
            if (strchr(";|&(", c))
                cmd = true;
            else if (c == '<' || c == '>')
                cmd = false;
            start = i + 1;
            continue;
        }
        if (!in_word) {
            in_word = true;
            word_begin = i;
        }
    }
    *cmdpos = cmd;
    return in_word ? word_begin : start;
}

static void list_columns(ed_t *e, comp_t *c)
{
    int n = c->names.n;
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)utf8_str_width(c->names.v[i], strlen(c->names.v[i])) +
                (c->isdir[i] ? 1 : 0);
        if (w > maxw) maxw = w;
    }
    int colw = maxw + 2;
    int ncols = e->cols / colw;
    if (ncols < 1) ncols = 1;
    int nrows = (n + ncols - 1) / ncols;
    /* sorted listing, down the columns like ls */
    char **idx = xmalloc((size_t)n * sizeof(char *));
    for (int i = 0; i < n; i++) idx[i] = c->names.v[i];
    sort_strs(idx, n);
    strbuf_t *o = &e->out;
    for (int r = 0; r < nrows; r++) {
        for (int k = 0; k < ncols; k++) {
            int i = k * nrows + r;
            if (i >= n) break;
            const char *nm = idx[i];
            bool isdir = false;
            for (int m = 0; m < n; m++)
                if (c->names.v[m] == nm) isdir = c->isdir[m];
            sb_puts(o, nm);
            if (isdir) sb_putc(o, '/');
            int w = (int)utf8_str_width(nm, strlen(nm)) + (isdir ? 1 : 0);
            if (k < ncols - 1 && i + nrows < n)
                for (int s = w; s < colw; s++) sb_putc(o, ' ');
        }
        sb_puts(o, "\r\n");
    }
    xfree(idx);
}

static void ed_redraw_all(ed_t *e);

static void complete(ed_t *e)
{
    bool cmdpos;
    size_t ws = comp_word_start(e, &cmdpos);
    const char *s = e->buf.s ? e->buf.s : "";
    char *word = comp_unquote(s + ws, e->cur - ws);
    comp_t c;
    memset(&c, 0, sizeof c);
    bool var = false;
    const char *dollar = strrchr(word, '$');
    if (dollar && !strchr(dollar, '/')) {
        bool braced = dollar[1] == '{';
        const char *nm = dollar + 1 + (braced ? 1 : 0);
        bool ok = true;
        for (const char *p = nm; *p; p++)
            if (!is_name_char((u8)*p)) ok = false;
        if (ok) {
            var = true;
            comp_vars(&c, nm, braced);
            /* keep what came before the $ */
            size_t pre = (size_t)(dollar - word);
            for (int i = 0; i < c.inserts.n; i++) {
                strbuf_t b = {0};
                comp_quote(&b, xstrndup(word, pre));
                sb_puts(&b, c.inserts.v[i]);
                xfree(c.inserts.v[i]);
                c.inserts.v[i] = sb_take(&b);
            }
        }
    }
    if (!var) {
        if (word[0] == '~' && !strchr(word, '/')) {
            comp_users(&c, word + 1);
        } else if (cmdpos && !strchr(word, '/')) {
            comp_commands(&c, word);
            if (shopt_autocd || c.names.n == 0)
                comp_files(&c, word, true, false);
        } else {
            /* cd and pushd want directories */
            bool dirs = false;
            const char *t = s;
            while (*t == ' ') t++;
            if ((strncmp(t, "cd ", 3) == 0 || strncmp(t, "pushd ", 6) == 0 ||
                 strncmp(t, "rmdir ", 6) == 0) && ws > 0)
                dirs = true;
            comp_files(&c, word, dirs, cmdpos);
        }
    }
    int n = c.names.n;
    if (n == 0) {
        sb_putc(&e->out, '\a');
        ed_flush(e);
        comp_free(&c);
        xfree(word);
        return;
    }
    if (n == 1) {
        ed_undo_push(e);
        ed_delete(e, ws, e->cur, false);
        const char *ins = c.inserts.v[0];
        ed_insert(e, ins, strlen(ins));
        if (c.isdir[0]) {
            if (e->buf.s[e->cur - 1] != '/')
                ed_insert(e, "/", 1);
        } else if (!var || true) {
            if (e->cur >= e->buf.len || e->buf.s[e->cur] != ' ')
                ed_insert(e, " ", 1);
        }
        e->last_was_tab = false;
        comp_free(&c);
        xfree(word);
        return;
    }
    /* several: the longest common prefix of what would be inserted */
    size_t lcp = strlen(c.inserts.v[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < lcp && c.inserts.v[i][k] == c.inserts.v[0][k])
            k++;
        lcp = k;
    }
    /* do not cut a UTF-8 character or a backslash-escape in half */
    while (lcp > 0 && ((u8)c.inserts.v[0][lcp] & 0xC0) == 0x80)
        lcp--;
    if (lcp > 0 && c.inserts.v[0][lcp - 1] == '\\') {
        size_t bs = 0;
        while (bs < lcp && c.inserts.v[0][lcp - 1 - bs] == '\\') bs++;
        if (bs % 2) lcp--;
    }
    size_t typed = e->cur - ws;
    if (lcp > typed || (lcp == typed && strncmp(s + ws, c.inserts.v[0], lcp) != 0)) {
        ed_undo_push(e);
        ed_delete(e, ws, e->cur, false);
        ed_insert(e, c.inserts.v[0], lcp);
        e->last_was_tab = false;
        comp_free(&c);
        xfree(word);
        return;
    }
    if (!e->last_was_tab) {
        sb_putc(&e->out, '\a');
        ed_flush(e);
        e->last_was_tab = true;
        comp_free(&c);
        xfree(word);
        return;
    }
    /* second Tab: show them */
    ed_finish_line(e);
    if (n > 100) {
        outf(out2, "Display all %d possibilities? (y or n)", n);
        flush_out(out2);
        int k;
        for (;;) {
            char seq[8];
            int sl;
            k = read_key(e->ifd, seq, &sl);
            if (k == 'y' || k == 'Y' || k == ' ' || k == 'n' || k == 'N' ||
                k == 3 || k == 7 || k == 127 || k == K_EOF || k == K_INTR)
                break;
        }
        sb_puts(&e->out, "\r\n");
        if (k != 'y' && k != 'Y' && k != ' ') {
            ed_redraw_all(e);
            comp_free(&c);
            xfree(word);
            return;
        }
    }
    list_columns(e, &c);
    ed_flush(e);
    ed_redraw_all(e);
    comp_free(&c);
    xfree(word);
}

/* ── incremental search (Ctrl-R) ───────────────────────────────────── */

static void search_draw(ed_t *e, const char *q, int idx, bool failed, bool fwd)
{
    strbuf_t *o = &e->out;
    if (e->cur_row > 0)
        sb_printf(o, "\x1b[%dA", e->cur_row);
    sb_puts(o, "\r\x1b[J");
    strbuf_t line = {0};
    sb_printf(&line, "(%s%s-i-search)`%s': ", failed ? "failed " : "",
              fwd ? "" : "reverse", q);
    const char *m = idx >= 0 && idx < hist.n ? hist.v[idx] : "";
    /* show it through the same layout code, as a prompt of its own */
    char *save_prompt = e->prompt_last;
    int save_pw = e->pw;
    e->prompt_last = line.s;
    e->pw = (int)utf8_str_width(line.s, line.len);
    strbuf_t save_buf = e->buf;
    size_t save_cur = e->cur;
    memset(&e->buf, 0, sizeof e->buf);
    sb_puts(&e->buf, m);
    const char *hit = *q ? strstr(m, q) : NULL;
    e->cur = hit ? (size_t)(hit - m) : e->buf.len;
    e->cur_row = 0;
    sb_puts(o, line.s);
    for (size_t i = 0; i < e->buf.len; ) {
        char look[8];
        size_t used, ll;
        char_look(e->buf.s, e->buf.len, i, &used, look, &ll);
        sb_putn(o, look, ll);
        i += used;
    }
    int er, ec, cr, cc;
    layout_pos(e, e->buf.len, &er, &ec);
    if (ec == 0 && er > 0 && e->buf.len)
        sb_puts(o, "\r\n");
    layout_pos(e, e->cur, &cr, &cc);
    if (er > cr) sb_printf(o, "\x1b[%dA", er - cr);
    sb_putc(o, '\r');
    if (cc > 0) sb_printf(o, "\x1b[%dC", cc);
    e->cur_row = cr;
    sb_free(&e->buf);
    e->buf = save_buf;
    e->cur = save_cur;
    e->prompt_last = save_prompt;
    e->pw = save_pw;
    sb_free(&line);
    ed_flush(e);
}

/* Returns the key that ended the search (to be handled by the caller),
 * 0 when the search was accepted into the line, -1 when cancelled. */
static int isearch(ed_t *e, bool fwd)
{
    strbuf_t q = {0};
    sb_str(&q);
    int idx = hist.n;
    int found = -1;
    bool failed = false;
    char *orig = xstrdup(e->buf.s ? e->buf.s : "");
    size_t orig_cur = e->cur;
    search_draw(e, "", -1, false, fwd);
    int ret = 0;
    for (;;) {
        char seq[8];
        int sl;
        int k = read_key(e->ifd, seq, &sl);
        if (k == K_WINCH) {
            got_sigwinch = 0;
            term_update_size();
            e->cols = term_cols;
            search_draw(e, q.s, found, failed, fwd);
            continue;
        }
        if (k == 18 || k == 19) {           /* ^R again / ^S */
            fwd = k == 19;
            int from = found >= 0 ? found : idx;
            int step = fwd ? 1 : -1;
            int i = from + step;
            failed = true;
            for (; i >= 0 && i < hist.n; i += step)
                if (q.len && strstr(hist.v[i], q.s)) {
                    found = i;
                    failed = false;
                    break;
                }
            search_draw(e, q.s, found, failed, fwd);
            continue;
        }
        if (k == 127 || k == 8) {
            if (q.len) {
                q.len = utf8_prev(q.s, q.len);
                q.s[q.len] = '\0';
            }
            found = -1;
            failed = false;
            for (int i = hist.n - 1; i >= 0 && q.len; i--)
                if (strstr(hist.v[i], q.s)) { found = i; break; }
            if (q.len && found < 0) failed = true;
            search_draw(e, q.s, found, failed, fwd);
            continue;
        }
        if (k == 7 || k == 3 || k == 27) {  /* ^G ^C ESC: cancel */
            ed_set(e, orig);
            e->cur = orig_cur;
            ret = k == 3 ? 3 : -1;
            break;
        }
        if (k == K_EOF || k == K_INTR) {
            ret = k;
            break;
        }
        if (k < 0x110000 && k >= 0x20 && k != 127) {
            sb_putn(&q, seq, (size_t)sl);
            int start = found >= 0 ? found : hist.n - 1;
            found = -1;
            for (int i = start; i >= 0; i--)
                if (strstr(hist.v[i], q.s)) { found = i; break; }
            failed = found < 0;
            search_draw(e, q.s, found, failed, fwd);
            continue;
        }
        /* anything else accepts the match and is then handled */
        if (found >= 0) {
            ed_set(e, hist.v[found]);
            const char *hit = strstr(hist.v[found], q.s);
            if (hit && k != '\r' && k != '\n')
                e->cur = (size_t)(hit - hist.v[found]);
            e->hpos = found;
        }
        ret = k;
        break;
    }
    sb_free(&q);
    xfree(orig);
    return ret;
}

/* ── the editor ────────────────────────────────────────────────────── */

static void ed_redraw_all(ed_t *e)
{
    sb_puts(&e->out, "\r");
    /* the lines of the prompt before its last */
    size_t full = strlen(e->prompt_all), last = strlen(e->prompt_last);
    for (size_t i = 0; i + last < full; i++) {
        char ch = e->prompt_all[i];
        if (ch == '\n')
            sb_puts(&e->out, "\r\n");
        else
            sb_putc(&e->out, ch);
    }
    e->cur_row = 0;
    ed_refresh(e);
}

/* Split the expanded prompt into the part that is shown once and its
 * last line, and measure that line. \001..\002 wrap invisible runs (the
 * \[ \] of PS1); escape sequences outside them are skipped too, so a
 * prompt written without \[ \] does not throw the cursor off. */
static void prompt_measure(const char *raw, char **all, char **lastline, int *width)
{
    strbuf_t a = {0};
    for (const char *p = raw; *p; p++)
        if (*p != '\001' && *p != '\002')
            sb_putc(&a, *p);
    *all = sb_take(&a);
    const char *nl = strrchr(raw, '\n');
    const char *ls = nl ? nl + 1 : raw;
    strbuf_t l = {0};
    int w = 0;
    bool hidden = false;
    for (const char *p = ls; *p; ) {
        u8 c = (u8)*p;
        if (c == 1) { hidden = true; p++; continue; }
        if (c == 2) { hidden = false; p++; continue; }
        if (c == 27 && !hidden) {
            /* CSI ... final, or OSC ... BEL/ST */
            const char *st = p;
            p++;
            if (*p == '[') {
                p++;
                while (*p && !((u8)*p >= 0x40 && (u8)*p <= 0x7e)) p++;
                if (*p) p++;
            } else if (*p == ']') {
                while (*p && *p != 7 && !(*p == 27 && p[1] == '\\')) p++;
                if (*p == 7) p++;
                else if (*p) p += 2;
            } else if (*p) {
                p++;
            }
            sb_putn(&l, st, (size_t)(p - st));
            continue;
        }
        if (c == '\r') { w = 0; sb_putc(&l, (char)c); p++; continue; }
        int n = 1;
        u32 cp = c < 0x80 ? c : utf8_decode(p, strlen(p), &n);
        if (n < 1) n = 1;
        sb_putn(&l, p, (size_t)n);
        if (!hidden && cp >= 0x20 && cp != 0x7f)
            w += utf8_width(cp);
        p += n;
    }
    *lastline = sb_take(&l);
    *width = w;
}

static char *prompt_expand_full(const char *ps)
{
    int dummy;
    char *s1 = prompt_expand(ps, &dummy);
    char *s2 = expand_prompt_var(s1);
    xfree(s1);
    return s2;
}

/* The fallback when the terminal cannot be put in raw mode: print the
 * prompt and read a line the plain way. */
static long plain_read_line(int fd, const char *prompt, strbuf_t *out)
{
    char *p = prompt_expand_full(prompt);
    char *all, *last;
    int w;
    prompt_measure(p, &all, &last, &w);
    xwrite_all(2, all, strlen(all));
    xfree(p);
    xfree(all);
    xfree(last);
    bool eof;
    out->len = 0;
    if (!read_line_fd(fd, out, '\n', &eof) && eof)
        return EDIT_EOF;
    return (long)out->len;
}

static bool osc7_terminal(void)
{
    const char *t = var_get("TERM");
    if (!t)
        return false;
    return strncmp(t, "foot", 4) == 0 || strncmp(t, "xterm", 5) == 0 ||
           strncmp(t, "alacritty", 9) == 0 || strncmp(t, "kitty", 5) == 0 ||
           strncmp(t, "wezterm", 7) == 0 || strncmp(t, "vte", 3) == 0 ||
           strncmp(t, "tmux", 4) == 0;
}

/* Tell a terminal that understands it where we are: the window title,
 * and OSC 7 so a new terminal window can open in the same directory. */
static void announce_cwd(int fd)
{
    if (!osc7_terminal())
        return;
    const char *pwd = var_get("PWD");
    if (!pwd)
        return;
    char uts[6 * 65];
    char host[65] = "localhost";
    if (lp_uname(uts) >= 0 && uts[65])
        strlcpy(host, uts + 65, sizeof host);
    strbuf_t b = {0};
    sb_printf(&b, "\x1b]7;file://%s", host);
    for (const char *p = pwd; *p; p++) {
        u8 c = (u8)*p;
        if (c < 0x21 || c >= 0x7f || c == '%')
            sb_printf(&b, "%%%02X", c);
        else
            sb_putc(&b, (char)c);
    }
    sb_puts(&b, "\x1b\\");
    xwrite_all(fd, b.s, b.len);
    sb_free(&b);
}

static long edit_read_line(const char *ps, strbuf_t *out)
{
    int ifd = 0, ofd = 2;
    if (!lp_isatty(0))
        return plain_read_line(0, ps, out);
    if (!lp_isatty(ofd))
        ofd = lp_isatty(1) ? 1 : ofd;
    lp_termios_t saved;
    if (lp_term_raw(ifd, &saved) < 0)
        return plain_read_line(ifd, ps, out);
    lp_signal_handler(28, on_sigwinch);
    term_update_size();
    got_sigwinch = 0;

    ed_t e;
    memset(&e, 0, sizeof e);
    e.ifd = ifd;
    e.ofd = ofd;
    e.cols = term_cols;
    e.hpos = hist.n;
    sb_str(&e.buf);
    char *expanded = prompt_expand_full(ps);
    prompt_measure(expanded, &e.prompt_all, &e.prompt_last, &e.pw);
    xfree(expanded);

    if (ps1_is_primary)
        announce_cwd(ofd);
    ed_redraw_all(&e);
    long result = 0;
    for (;;) {
        char seq[8];
        int sl;
        if (got_sigwinch) {
            got_sigwinch = 0;
            term_update_size();
            e.cols = term_cols;
            ed_refresh(&e);
        }
        int k = read_key(ifd, seq, &sl);
        bool was_tab = e.last_was_tab;
        bool was_kill = e.last_was_kill;
        bool was_lastarg = e.last_was_lastarg;
        e.last_was_tab = false;
        e.last_was_kill = false;
        e.last_was_lastarg = false;
again:
        if (k == K_WINCH)
            continue;
        if (k == K_EOF) {
            result = EDIT_EOF;
            break;
        }
        if (k == K_INTR) {
            /* a hangup or a trapped signal while at the prompt */
            ed_finish_line(&e);
            result = EDIT_INTR;
            break;
        }
        if (k == 18 || k == 19) {               /* ^R ^S */
            int r = isearch(&e, k == 19);
            if (r == 3) {
                sb_puts(&e.out, "^C");
                ed_finish_line(&e);
                result = EDIT_INTR;
                break;
            }
            if (r == -1 || r == 0) {
                ed_refresh(&e);
                continue;
            }
            e.cur_row = e.cur_row;
            ed_refresh(&e);
            k = r;
            goto again;
        }
        switch (k) {
        case '\r':
        case '\n':
            e.cur = e.buf.len;
            ed_refresh(&e);
            ed_finish_line(&e);
            result = (long)e.buf.len;
            goto done;
        case 3:                                 /* ^C */
            sb_puts(&e.out, "^C");
            e.cur = e.buf.len;
            ed_refresh(&e);
            sb_puts(&e.out, "^C");
            ed_finish_line(&e);
            result = EDIT_INTR;
            goto done;
        case 4:                                 /* ^D */
            if (e.buf.len == 0) {
                result = EDIT_EOF;
                goto done;
            }
            if (e.cur < e.buf.len) {
                ed_undo_push(&e);
                ed_delete(&e, e.cur, next_char(&e, e.cur), false);
            }
            break;
        case K_DEL:
            if (e.cur < e.buf.len) {
                ed_undo_push(&e);
                ed_delete(&e, e.cur, next_char(&e, e.cur), false);
            }
            break;
        case 127:
        case 8:                                 /* backspace */
            if (e.cur > 0) {
                ed_undo_push(&e);
                ed_delete(&e, prev_char(&e, e.cur), e.cur, false);
            }
            break;
        case 1: case K_HOME:
            e.cur = 0;
            break;
        case 5: case K_END:
            e.cur = e.buf.len;
            break;
        case 2: case K_LEFT:
            e.cur = prev_char(&e, e.cur);
            break;
        case 6: case K_RIGHT:
            e.cur = next_char(&e, e.cur);
            break;
        case K_ALT | 'b': case K_CLEFT:
            e.cur = word_left(&e, e.cur);
            break;
        case K_ALT | 'f': case K_CRIGHT:
            e.cur = word_right(&e, e.cur);
            break;
        case 11:                                /* ^K */
            ed_undo_push(&e);
            e.last_was_kill = was_kill;
            ed_delete(&e, e.cur, e.buf.len, true);
            e.last_was_kill = true;
            break;
        case 21:                                /* ^U */
            ed_undo_push(&e);
            e.last_was_kill = was_kill;
            ed_delete(&e, 0, e.cur, true);
            e.last_was_kill = true;
            break;
        case 23: {                              /* ^W: back to a blank */
            size_t i = e.cur;
            while (i > 0 && (e.buf.s[i - 1] == ' ' || e.buf.s[i - 1] == '\t'))
                i--;
            while (i > 0 && e.buf.s[i - 1] != ' ' && e.buf.s[i - 1] != '\t')
                i--;
            ed_undo_push(&e);
            e.last_was_kill = was_kill;
            ed_delete(&e, i, e.cur, true);
            e.last_was_kill = true;
            break;
        }
        case K_ALT | 127: {                     /* Alt-Backspace */
            size_t i = word_left(&e, e.cur);
            ed_undo_push(&e);
            e.last_was_kill = was_kill;
            ed_delete(&e, i, e.cur, true);
            e.last_was_kill = true;
            break;
        }
        case K_ALT | 'd': case K_CDEL: {
            size_t i = word_right(&e, e.cur);
            ed_undo_push(&e);
            e.last_was_kill = was_kill;
            ed_delete(&e, e.cur, i, true);
            e.last_was_kill = true;
            break;
        }
        case 25:                                /* ^Y */
            if (e.killed) {
                ed_undo_push(&e);
                ed_insert(&e, e.killed, strlen(e.killed));
            }
            break;
        case 20: {                              /* ^T */
            if (e.cur == 0 || e.buf.len < 2)
                break;
            ed_undo_push(&e);
            size_t b = e.cur < e.buf.len ? e.cur : prev_char(&e, e.cur);
            size_t a = prev_char(&e, b);
            size_t c = next_char(&e, b);
            char *first = xstrndup(e.buf.s + a, b - a);
            char *second = xstrndup(e.buf.s + b, c - b);
            memcpy(e.buf.s + a, second, c - b);
            memcpy(e.buf.s + a + (c - b), first, b - a);
            xfree(first);
            xfree(second);
            e.cur = c;
            break;
        }
        case 31:                                /* ^_ undo */
        case K_ALT | 'u' | 0x1000: {
            if (e.undo.n) {
                e.undo.n--;
                ed_set(&e, e.undo.v[e.undo.n]);
                e.cur = e.undo_cur[e.undo.n];
                if (e.cur > e.buf.len) e.cur = e.buf.len;
                xfree(e.undo.v[e.undo.n]);
                e.undo.v[e.undo.n] = NULL;
            } else {
                sb_putc(&e.out, '\a');
            }
            break;
        }
        case 12:                                /* ^L */
            sb_puts(&e.out, "\x1b[H\x1b[2J");
            ed_redraw_all(&e);
            continue;
        case 16: case K_UP:                     /* ^P */
            if (e.hpos > 0) {
                if (e.hpos == hist.n) {
                    xfree(e.saved);
                    e.saved = xstrdup(e.buf.s ? e.buf.s : "");
                }
                e.hpos--;
                ed_set(&e, hist.v[e.hpos]);
            } else {
                sb_putc(&e.out, '\a');
            }
            break;
        case 14: case K_DOWN:                   /* ^N */
            if (e.hpos < hist.n) {
                e.hpos++;
                ed_set(&e, e.hpos == hist.n ? (e.saved ? e.saved : "") : hist.v[e.hpos]);
            } else {
                sb_putc(&e.out, '\a');
            }
            break;
        case K_ALT | '<': case K_PGUP:
            if (hist.n) {
                if (e.hpos == hist.n) {
                    xfree(e.saved);
                    e.saved = xstrdup(e.buf.s ? e.buf.s : "");
                }
                e.hpos = 0;
                ed_set(&e, hist.v[0]);
            }
            break;
        case K_ALT | '>': case K_PGDN:
            e.hpos = hist.n;
            ed_set(&e, e.saved ? e.saved : "");
            break;
        case K_ALT | '.': case K_ALT | '_': {
            /* the last word of the previous command; again for older */
            int back = was_lastarg ? e.lastarg_n + 1 : 1;
            int idx = hist.n - back;
            if (idx < 0) {
                sb_putc(&e.out, '\a');
                break;
            }
            strvec_t words = {0};
            hist_words(hist.v[idx], &words);
            ed_undo_push(&e);
            if (was_lastarg)
                ed_delete(&e, e.lastarg_at, e.lastarg_at + e.lastarg_len, false);
            if (words.n) {
                const char *w = words.v[words.n - 1];
                e.lastarg_at = e.cur;
                ed_insert(&e, w, strlen(w));
                e.lastarg_len = strlen(w);
            } else {
                e.lastarg_at = e.cur;
                e.lastarg_len = 0;
            }
            sv_free(&words);
            e.lastarg_n = back;
            e.last_was_lastarg = true;
            break;
        }
        case K_ALT | 'u': case K_ALT | 'l': case K_ALT | 'c': {
            size_t a = e.cur;
            while (a < e.buf.len && !is_word_byte((u8)e.buf.s[a])) a++;
            size_t b = word_right(&e, a);
            ed_undo_push(&e);
            for (size_t i = a; i < b; i++) {
                char ch = e.buf.s[i];
                bool up = k == (K_ALT | 'u') || (k == (K_ALT | 'c') && i == a);
                if (up && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                else if (!up && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
                e.buf.s[i] = ch;
            }
            e.cur = b;
            break;
        }
        case '\t':
            e.last_was_tab = was_tab;
            complete(&e);
            if (e.out.len)
                ed_flush(&e);
            break;
        case 22: {                              /* ^V: the next key as is */
            int d = read_byte(ifd, -1);
            if (d >= 0) {
                char ch = (char)d;
                ed_undo_push(&e);
                ed_insert(&e, &ch, 1);
            }
            break;
        }
        case 27:
        case 7:
        case K_NONE:
        case K_INS:
            break;
        default:
            if (k >= 0x20 && k < 0x110000 && k != 127) {
                if (e.undo.n == 0 || (sl == 1 && seq[0] == ' '))
                    ed_undo_push(&e);
                ed_insert(&e, seq, (size_t)sl);
            }
            break;
        }
        if (!input_pending(ifd))
            ed_refresh(&e);
    }
done:
    ed_flush(&e);
    lp_term_restore(ifd, &saved);
    if (result >= 0) {
        out->len = 0;
        sb_putn(out, e.buf.s ? e.buf.s : "", e.buf.len);
        sb_str(out);
    }
    sb_free(&e.buf);
    sb_free(&e.out);
    xfree(e.prompt_all);
    xfree(e.prompt_last);
    xfree(e.saved);
    xfree(e.killed);
    sv_free(&e.undo);
    xfree(e.undo_cur);
    return result;
}
