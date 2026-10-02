/* edit - a full-screen text editor you can write code in.
 *
 *   edit [-n] [-R] <file>   open it, creating it if it is not there
 *     -n   start with the line numbers showing
 *     -R   open read-only even when the file could be written
 *
 * Keys:
 *   arrows          move                 Ctrl-Left/Right  word left / right
 *   Home / End      start/end of line    PgUp / PgDn      one screen
 *   Ctrl-Home/End   start/end of file    Enter            split, keeping the indent
 *   Backspace / Del erase                Ctrl-K           cut this line
 *   Ctrl-A          start selecting      Ctrl-B copy   Ctrl-X cut   Ctrl-V paste
 *   Ctrl-W          search forward       Ctrl-P           search backward
 *   Ctrl-N          the same search again    Ctrl-R       search and replace
 *   Ctrl-T          go to line           Ctrl-D           line numbers on/off
 *   Ctrl-Z          undo                 Ctrl-Y           redo
 *   Ctrl-S          save                 Ctrl-O           save as
 *   Ctrl-G          help                 Ctrl-L           redraw
 *   Ctrl-Q, Ctrl-C  quit
 *
 * ── Why this exists ──
 * There was no editor, so changing a file meant retyping the whole thing
 * through 'cat > file'. You could not fix one line of configuration over
 * SSH. That first version could type and save; this one is meant for
 * writing a program in, which needs searching, undo and a clipboard.
 *
 * ── Drawing ──
 * The serial console runs at 115200 baud. Redrawing all of 80x24 is 1920
 * bytes, about 170ms, and typing visibly lags. So we remember what
 * changed and draw only that much:
 *   DIRTY_NONE  just move the cursor (arrow keys)
 *   DIRTY_LINE  redraw that one line (typing)
 *   DIRTY_ALL   everything (scrolling, adding or removing lines)
 * It also goes out in one write. Writing in pieces makes it flicker.
 *
 * ── Undo ──
 * Not snapshots of the buffer: a 10000-line file is around 300KB and
 * fifty of those is 15MB on a board with 512MB, for a feature nobody
 * would forgive us for making the machine swap.
 *
 * Instead each record is "put these lines back at this position", and it
 * only holds the lines the change actually touched - one line for a
 * keystroke, two for joining lines. Applying a record captures what it
 * replaced, which is exactly the record that undoes it again, so undo
 * and redo are the same code moving records between two stacks.
 *
 * Both stacks together are bounded twice: UNDO_MAX records and
 * UNDO_BYTES of text. Whichever runs out first, the oldest records go.
 * A change too big to fit at all throws the history away rather than
 * leaving records that describe a buffer that no longer exists - undo
 * that quietly corrupts the file is worse than no undo.
 *
 * ── Never lose the file ──
 * Saving writes a temporary in the same directory and renames it over
 * the original, so an interrupted save leaves the old file intact and a
 * reader sees one or the other, never half of each. The permissions of
 * the original are kept: editing /etc/rc must not leave it non-executable.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_LINES     20000
#define TAB_WIDTH     4
#define OUT_CAP       65536
#define RENDER_CAP    4096
#define UNDO_MAX      64
#define UNDO_BYTES    (192 * 1024)
#define CLIP_MAX      (256 * 1024)
#define PAT_MAX       128

/* How many bytes are waiting to be read. The same number on x86-64 and
 * arm64 - it comes from asm-generic - and it is the only way to tell a
 * bare ESC from the start of an arrow key without blocking. */
#define FIONREAD      0x541B

typedef struct {
    char  *text;      /* NUL terminated, never contains '\n' */
    size_t len;
    size_t cap;
} line_t;

static line_t lines[MAX_LINES];
static int    nlines = 0;

static int  cy = 0, cx = 0;        /* cursor: line, and byte within it */
static int  top = 0, left = 0;     /* what the top-left of the screen shows */
static int  rows = 24, cols = 80;  /* terminal size; the text gets rows-2 */
static bool modified = false;
static bool running  = true;
static bool readonly = false;
static bool show_numbers = false;

static char fname[512];            /* not argv: save-as changes it */
static char status[256];

typedef enum { DIRTY_NONE = 0, DIRTY_LINE, DIRTY_ALL } dirty_t;
static dirty_t dirty = DIRTY_ALL;

static lp_termios_t saved_term;

/* ── Output buffer ───────────────────────────────────────────────
 * Everything is gathered here so it leaves in a single write. */
static char   outbuf[OUT_CAP];
static size_t outlen = 0;

static void out(const char *s, size_t n)
{
    if (outlen + n > sizeof(outbuf))
        n = sizeof(outbuf) - outlen;
    memcpy(outbuf + outlen, s, n);
    outlen += n;
}
static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, int a, int b)
{
    char buf[64];
    int  n = snprintf(buf, sizeof(buf), fmt, a, b);
    if (n > 0)
        out(buf, (size_t)n);
}

static void move_to(int row, int col) { outf("\x1b[%d;%dH", row, col); }

static void flush_out(void)
{
    size_t off = 0;
    while (off < outlen) {
        long w = lp_write(STDOUT_FILENO, outbuf + off, outlen - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    outlen = 0;
}

static void set_status(const char *s) { strlcpy(status, s, sizeof(status)); }

/* ── Managing lines ────────────────────────────────────────────── */

static bool line_reserve(line_t *l, size_t need)
{
    if (l->cap >= need + 1)
        return true;
    size_t cap = l->cap ? l->cap : 64;
    while (cap < need + 1)
        cap *= 2;
    char *p = realloc(l->text, cap);
    if (!p)
        return false;
    l->text = p;
    l->cap  = cap;
    return true;
}

/* Insert an empty line at idx. true on success. */
static bool insert_line(int idx)
{
    if (nlines >= MAX_LINES)
        return false;
    for (int i = nlines; i > idx; i--)
        lines[i] = lines[i - 1];
    lines[idx].text = NULL;
    lines[idx].len  = 0;
    lines[idx].cap  = 0;
    if (!line_reserve(&lines[idx], 0))
        return false;
    lines[idx].text[0] = '\0';
    nlines++;
    return true;
}

/* The lines at[..at+n) joined by '\n'. This is what an undo record
 * holds; it is also how the clipboard is shaped. NULL when out of
 * memory, and *outlen is the length without a NUL. */
static char *lines_capture(int at, int n, size_t *outlen_p)
{
    size_t total = 1;
    for (int i = 0; i < n; i++)
        total += lines[at + i].len + 1;

    char *b = malloc(total);
    if (!b)
        return NULL;

    size_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(b + off, lines[at + i].text, lines[at + i].len);
        off += lines[at + i].len;
        if (i < n - 1)
            b[off++] = '\n';
    }
    b[off] = '\0';
    *outlen_p = off;
    return b;
}

/* Replace the nremove lines at `at` with the nadd lines held in blob.
 * This is the only thing that changes how many lines there are, so it is
 * the only place that has to get the shuffling right. */
static bool lines_splice(int at, int nremove, const char *blob,
                         size_t bloblen, int nadd)
{
    if (at < 0 || at > nlines)
        return false;
    if (nremove > nlines - at)
        nremove = nlines - at;
    if (nlines - nremove + nadd > MAX_LINES)
        return false;

    for (int i = at; i < at + nremove; i++)
        free(lines[i].text);

    int delta = nadd - nremove;
    if (delta > 0) {
        for (int i = nlines - 1; i >= at + nremove; i--)
            lines[i + delta] = lines[i];
    } else if (delta < 0) {
        for (int i = at + nremove; i < nlines; i++)
            lines[i + delta] = lines[i];
    }
    nlines += delta;

    size_t off = 0;
    for (int i = 0; i < nadd; i++) {
        size_t end = off;
        while (end < bloblen && blob[end] != '\n')
            end++;

        line_t *l = &lines[at + i];
        l->text = NULL;
        l->len  = 0;
        l->cap  = 0;
        if (!line_reserve(l, end - off))
            return false;
        memcpy(l->text, blob + off, end - off);
        l->text[end - off] = '\0';
        l->len = end - off;
        off = end + 1;
    }

    if (nlines == 0)
        insert_line(0);
    return true;
}

/* Clamp cx into the line, and onto a character boundary. Moving up or
 * down keeps the byte offset, which can land mid-character. */
static void clamp_cx(void)
{
    if (cy >= nlines) cy = nlines - 1;
    if (cy < 0)       cy = 0;

    const line_t *l = &lines[cy];
    if (cx > (int)l->len) cx = (int)l->len;
    if (cx < 0)           cx = 0;

    if (cx > 0 && cx < (int)l->len &&
        ((unsigned char)l->text[cx] & 0xC0) == 0x80)
        cx = (int)utf8_prev(l->text, (size_t)cx);
}

/* ── Undo and redo ─────────────────────────────────────────────── */

typedef struct {
    int    at;        /* first line the record covers */
    int    nadd;      /* lines held in blob */
    int    nremove;   /* lines it replaces when applied */
    size_t bloblen;
    char  *blob;
    int    cy, cx;    /* where the cursor was before the change */
} change_t;

typedef struct { change_t v[UNDO_MAX]; int start, count; } stack_t;

static stack_t undo_s, redo_s;
static size_t  hist_bytes = 0;
static int     typing_line = -1;   /* a run of typing already has a record */

static void stack_drop_oldest(stack_t *s)
{
    if (!s->count)
        return;
    hist_bytes -= s->v[s->start].bloblen;
    free(s->v[s->start].blob);
    s->start = (s->start + 1) % UNDO_MAX;
    s->count--;
}

static void stack_clear(stack_t *s) { while (s->count) stack_drop_oldest(s); }

/* false means it did not fit even after clearing everything else out. */
static bool stack_push(stack_t *s, const change_t *c)
{
    stack_t *other = (s == &undo_s) ? &redo_s : &undo_s;

    if (s->count == UNDO_MAX)
        stack_drop_oldest(s);
    while (hist_bytes + c->bloblen > UNDO_BYTES) {
        if (other->count)      stack_drop_oldest(other);
        else if (s->count)     stack_drop_oldest(s);
        else                   return false;
    }
    s->v[(s->start + s->count) % UNDO_MAX] = *c;
    s->count++;
    hist_bytes += c->bloblen;
    return true;
}

/* Call this BEFORE a change, saying which lines it is about to rewrite
 * and how many lines will stand there afterwards. */
static void record_change(int at, int nold, int mnew)
{
    typing_line = -1;
    stack_clear(&redo_s);          /* editing throws the redone future away */

    change_t c;
    c.at      = at;
    c.nadd    = nold;
    c.nremove = mnew;
    c.cy      = cy;
    c.cx      = cx;
    c.blob    = lines_capture(at, nold, &c.bloblen);

    if (!c.blob || !stack_push(&undo_s, &c)) {
        free(c.blob);
        stack_clear(&undo_s);
        set_status("that change is too big to undo - the history was cleared");
    }
}

/* One record per run of typing on a line, not one per keystroke. */
static void record_typing(void)
{
    if (typing_line == cy)
        return;
    record_change(cy, 1, 1);
    typing_line = cy;
}

static void apply_change(change_t *c, stack_t *dest)
{
    change_t inv;
    inv.at      = c->at;
    inv.nadd    = c->nremove;
    inv.nremove = c->nadd;
    inv.cy      = cy;
    inv.cx      = cx;
    inv.blob    = lines_capture(c->at, c->nremove, &inv.bloblen);

    lines_splice(c->at, c->nremove, c->blob, c->bloblen, c->nadd);

    if (!inv.blob || !stack_push(dest, &inv)) {
        free(inv.blob);
        stack_clear(dest);
    }
    free(c->blob);

    cy = c->cy;
    cx = c->cx;
    clamp_cx();
    modified = true;
    dirty = DIRTY_ALL;
}

static void do_undo(void)
{
    if (!undo_s.count) { set_status("nothing to undo"); return; }
    change_t c = undo_s.v[(undo_s.start + undo_s.count - 1) % UNDO_MAX];
    hist_bytes -= c.bloblen;
    undo_s.count--;
    apply_change(&c, &redo_s);
    typing_line = -1;
}

static void do_redo(void)
{
    if (!redo_s.count) { set_status("nothing to redo"); return; }
    change_t c = redo_s.v[(redo_s.start + redo_s.count - 1) % UNDO_MAX];
    hist_bytes -= c.bloblen;
    redo_s.count--;
    apply_change(&c, &undo_s);
    typing_line = -1;
}

/* ── Files ─────────────────────────────────────────────────────── */

/* Can we write to this path? For a file that is not there yet the
 * question is really about its directory. Asked at OPEN time, because
 * finding out at save time means finding out after the typing. */
static bool path_writable(const char *path)
{
    if (lp_exists(path))
        return lp_access(path, W_OK) == 0;

    char dir[512];
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash)      strlcpy(dir, ".", sizeof(dir));
    else if (slash == dir) dir[1] = '\0';
    else             *slash = '\0';
    return lp_access(dir, W_OK) == 0;
}

static bool load_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        /* Missing? Start empty - we are creating it. */
        insert_line(0);
        set_status("new file");
        return true;
    }

    if (!insert_line(0)) { lp_close((int)fd); return false; }

    char buf[8192];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (!insert_line(nlines)) { lp_close((int)fd); return false; }
                continue;
            }
            if (ch == '\r')
                continue;                   /* read CRLF files too */
            line_t *l = &lines[nlines - 1];
            if (!line_reserve(l, l->len + 1)) { lp_close((int)fd); return false; }
            l->text[l->len++] = ch;
            l->text[l->len]   = '\0';
        }
    }
    lp_close((int)fd);

    /* A file ending in a newline leaves one empty line here. That is
     * right: writing it back puts the newline on again and nothing else. */
    char msg[128];
    snprintf(msg, sizeof(msg), "%d lines", nlines);
    set_status(msg);
    return true;
}

static bool save_to(const char *path)
{
    /* Keep the permissions of the file we are replacing. Creating the
     * new one 0644 unconditionally is how an editor turns /etc/rc into
     * a file the system will no longer run. */
    lp_stat_t st;
    mode_t mode = 0644;
    if (lp_stat(path, &st, true) == 0)
        mode = (mode_t)(st.mode & 07777);

    /* The temporary goes in the same directory: rename only works
     * within one filesystem, and a save must not stop being atomic
     * because /tmp is somewhere else. */
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        set_status("path too long");
        return false;
    }

    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "cannot write %s (%d) - Ctrl-O saves it somewhere else",
                 tmp, (int)-fd);
        set_status(msg);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < nlines && ok; i++) {
        size_t off = 0;
        while (off < lines[i].len) {
            long w = lp_write((int)fd, lines[i].text + off, lines[i].len - off);
            if (w <= 0) { ok = false; break; }
            off += (size_t)w;
        }
        /* No newline after the last line, so the line count is the same
         * when we read it back. */
        if (ok && i < nlines - 1) {
            if (lp_write((int)fd, "\n", 1) != 1)
                ok = false;
        }
    }
    lp_close((int)fd);

    if (!ok) {
        lp_unlink(tmp);
        set_status("write error - the original is untouched (disk full?)");
        return false;
    }

    long r = lp_rename(tmp, path);
    if (r < 0) {
        lp_unlink(tmp);
        char msg[160];
        snprintf(msg, sizeof(msg), "rename failed (%d) - nothing was changed",
                 (int)-r);
        set_status(msg);
        return false;
    }

    lp_sync();          /* the power can go at any moment on an SD card */
    modified = false;
    char msg[200];
    snprintf(msg, sizeof(msg), "saved %s (%d lines)", path, nlines);
    set_status(msg);
    return true;
}

/* ── The selection ─────────────────────────────────────────────── */

static bool mark_on = false;
static int  my = 0, mx = 0;

/* The selection in reading order. false when there is nothing in it. */
static bool sel_bounds(int *y0, int *x0, int *y1, int *x1)
{
    if (!mark_on)
        return false;
    if (my < cy || (my == cy && mx <= cx)) {
        *y0 = my; *x0 = mx; *y1 = cy; *x1 = cx;
    } else {
        *y0 = cy; *x0 = cx; *y1 = my; *x1 = mx;
    }
    return !(*y0 == *y1 && *x0 == *x1);
}

/* The byte range of line idx that is selected, and whether the line
 * break at its end is in the selection too. */
static bool sel_on_line(int idx, int *b0, int *b1, bool *eol)
{
    int y0, x0, y1, x1;
    *eol = false;
    if (!sel_bounds(&y0, &x0, &y1, &x1))
        return false;
    if (idx < y0 || idx > y1)
        return false;
    *b0  = (idx == y0) ? x0 : 0;
    *b1  = (idx == y1) ? x1 : (int)lines[idx].len;
    *eol = (idx < y1);
    return true;
}

/* ── The screen ────────────────────────────────────────────────── */

/* Width of the line-number gutter, 0 when they are off. It never
 * narrows below three digits, so the text does not shuffle sideways
 * every time the file crosses 10 or 100 lines. */
static int gutter_w(void)
{
    if (!show_numbers)
        return 0;
    int d = 1, n = nlines;
    while (n >= 10) { n /= 10; d++; }
    if (d < 3)          d = 3;
    if (d > cols / 2)   d = cols / 2;
    return d + 1;
}

static int text_cols(void)
{
    int c = cols - gutter_w();
    return c < 8 ? 8 : c;
}

/* The screen column the cursor sits at.
 *
 * cx is a byte offset; this is a screen column. They differ: a tab is
 * several columns, and one Hangul character is 3 bytes and 2 columns. */
static int screen_col(void)
{
    const line_t *l = &lines[cy];
    int col = 0;
    for (int i = 0; i < cx && i < (int)l->len; ) {
        if (l->text[i] == '\t') {
            col = (col / TAB_WIDTH + 1) * TAB_WIDTH;
            i++;
            continue;
        }
        int used;
        u32 cp = utf8_decode(l->text + i, l->len - (size_t)i, &used);
        if (used == 0) break;
        col += utf8_width(cp);
        i   += used;
    }
    return col;
}

/* Draw one line: the number, then the text, expanding tabs, honouring
 * the horizontal scroll and reversing whatever is selected. */
static void draw_line(int idx, int screen_row)
{
    move_to(screen_row + 1, 1);
    outs("\x1b[K");                        /* clear to end of line */

    if (idx >= nlines)
        return;

    int gw = gutter_w();
    if (gw) {
        char num[24];
        int n = snprintf(num, sizeof(num), "%*d ", gw - 1, idx + 1);
        if (n > 0)
            out(num, (size_t)n);
    }

    int  b0 = 0, b1 = 0;
    bool eol = false;
    bool sel = sel_on_line(idx, &b0, &b1, &eol);
    bool rev = false;

    /* Draw only what falls in screen columns [left, left+width).
     *
     * Cutting by bytes would be wrong: Hangul is 3 bytes and 2 columns,
     * so a byte cut lands mid-character, prints a broken glyph and
     * throws the line length off. Count columns, copy whole characters. */
    const line_t *l = &lines[idx];
    char   render[RENDER_CAP];
    size_t r     = 0;
    int    col   = 0;
    int    width = text_cols();

    /* The margin is what one turn of the loop can add at most: four
     * bytes of escape, four bytes of character, and a tab's worth of
     * spaces - plus the reset at the end. */
    for (size_t i = 0; i < l->len && r < sizeof(render) - 32; ) {
        bool want = sel && (int)i >= b0 && (int)i < b1;

        if (l->text[i] == '\t') {
            int stop = (col / TAB_WIDTH + 1) * TAB_WIDTH;
            while (col < stop) {
                if (col >= left && col < left + width) {
                    if (want != rev) {
                        memcpy(render + r, want ? "\x1b[7m" : "\x1b[0m", 4);
                        r += 4;
                        rev = want;
                    }
                    render[r++] = ' ';
                }
                col++;
            }
            i++;
            continue;
        }

        int used;
        u32 cp = utf8_decode(l->text + i, l->len - (size_t)i, &used);
        if (used == 0)
            break;
        int w = utf8_width(cp);

        /* Stop if the character would cross the right edge: half of one
         * is garbage. */
        if (col + w > left + width)
            break;

        if (col + w > left) {
            if (want != rev) {
                memcpy(render + r, want ? "\x1b[7m" : "\x1b[0m", 4);
                r += 4;
                rev = want;
            }
            if ((unsigned char)l->text[i] < 32) {
                render[r++] = '?';           /* a control character holds its column */
            } else if (col >= left) {
                memcpy(render + r, l->text + i, (size_t)used);
                r += (size_t)used;
            } else {
                /* A wide character straddling the left edge. We cannot
                 * draw half of it, so a space holds the column. */
                render[r++] = ' ';
            }
        }

        col += w;
        i   += used;
    }

    /* Show the line break itself as selected, or a multi-line selection
     * looks like it stops at the last character of each line. */
    if (eol && col >= left && col < left + width && r < sizeof(render) - 8) {
        if (!rev) { memcpy(render + r, "\x1b[7m", 4); r += 4; rev = true; }
        render[r++] = ' ';
    }
    if (rev) { memcpy(render + r, "\x1b[0m", 4); r += 4; }

    out(render, r);
}

static void draw_status(void)
{
    move_to(rows - 1, 1);
    outs("\x1b[K\x1b[7m");                 /* reverse video */

    char bar[320];
    int n = snprintf(bar, sizeof(bar),
                     " %s%s%s  line %d/%d  col %d%s",
                     fname,
                     modified ? " *" : "",
                     readonly ? "  [read-only]" : "",
                     cy + 1, nlines, screen_col() + 1,
                     mark_on ? "  [selecting]" : "");
    if (n > cols) n = cols;
    out(bar, (size_t)n);
    for (int i = n; i < cols; i++) outs(" ");
    outs("\x1b[0m");

    move_to(rows, 1);
    outs("\x1b[K");
    if (status[0])
        outs(status);
    else
        outs("Ctrl-G help   Ctrl-S save   Ctrl-Q quit");
}

static void place_cursor(void)
{
    move_to(cy - top + 1, gutter_w() + screen_col() - left + 1);
}

/* Scroll the view to follow the cursor if it left the screen.
 * Returns true if it moved, which means a full redraw. */
static bool scroll_to_cursor(void)
{
    int body = rows - 2;
    int before_top = top, before_left = left;

    if (cy < top)            top = cy;
    if (cy >= top + body)    top = cy - body + 1;
    if (top > nlines - 1)    top = nlines - 1;
    if (top < 0)             top = 0;

    int width = text_cols();
    int sc = screen_col();
    if (sc < left)           left = sc;
    if (sc >= left + width)  left = sc - width + 1;
    if (left < 0)            left = 0;

    return top != before_top || left != before_left;
}

/* The terminal can be resized under us at any moment and nothing tells
 * us. Asking costs one ioctl per keystroke, which is nothing, and the
 * alternative is a screen drawn to the old size - lines wrapping into
 * each other and a status bar in the middle of the text. */
static void check_size(void)
{
    int r = rows, c = cols;
    lp_term_size(STDOUT_FILENO, &r, &c);
    if (r < 4)  r = 4;
    if (c < 20) c = 20;
    if (r != rows || c != cols) {
        rows = r;
        cols = c;
        outs("\x1b[2J");
        dirty = DIRTY_ALL;
    }
}

static void refresh(void)
{
    check_size();
    if (scroll_to_cursor())
        dirty = DIRTY_ALL;
    if (mark_on)
        dirty = DIRTY_ALL;             /* the highlight follows the cursor */

    outs("\x1b[?25l");                 /* hide the cursor while drawing */

    if (dirty == DIRTY_ALL) {
        int body = rows - 2;
        for (int i = 0; i < body; i++)
            draw_line(top + i, i);
        draw_status();
    } else if (dirty == DIRTY_LINE) {
        draw_line(cy, cy - top);
        draw_status();
    } else {
        draw_status();                 /* the cursor position changed */
    }

    place_cursor();
    outs("\x1b[?25h");
    flush_out();
    dirty = DIRTY_NONE;
}

/* ── Key input ───────────────────────────────────────────────────
 * Arrow keys arrive as several bytes, like ESC [ A. We read the whole
 * sequence and turn it into one number of our own. */
enum {
    KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL,
    KEY_FILE_TOP, KEY_FILE_END, KEY_WORD_LEFT, KEY_WORD_RIGHT
};

/* Is the rest of an escape sequence on its way?
 *
 * The terminal gives us one byte at a time and a plain ESC looks exactly
 * like the first byte of an arrow key. Reading the next byte to find out
 * blocks forever when the answer is "there is no next byte", which is
 * how pressing ESC used to wedge the editor. So we ask how much is
 * waiting instead. 20ms is a long time for the two bytes behind an arrow
 * key - at 115200 baud they are 170us away - and short enough that ESC
 * feels immediate. */
static bool esc_more(void)
{
    int n = 0;
    if (lp_ioctl(STDIN_FILENO, FIONREAD, &n) < 0)
        return true;                   /* cannot ask: block, as before */
    if (n > 0)
        return true;
    lp_sleep_ms(20);
    if (lp_ioctl(STDIN_FILENO, FIONREAD, &n) < 0)
        return true;
    return n > 0;
}

static bool esc_byte(char *c)
{
    if (!esc_more())
        return false;
    return lp_read(STDIN_FILENO, c, 1) == 1;
}

static int read_key(void)
{
    char c;
    long n;
    do {
        n = lp_read(STDIN_FILENO, &c, 1);
    } while (n == 0);
    if (n < 0)
        return -1;

    if (c != 0x1b)
        return (unsigned char)c;

    char seq[8];
    if (!esc_byte(&seq[0])) return 0x1b;
    if (!esc_byte(&seq[1])) return 0x1b;

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            /* ESC [ n ~   or   ESC [ 1 ; 5 C (Ctrl and an arrow) */
            if (!esc_byte(&seq[2])) return 0x1b;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1': case '7': return KEY_HOME;
                case '3':           return KEY_DEL;
                case '4': case '8': return KEY_END;
                case '5':           return KEY_PGUP;
                case '6':           return KEY_PGDN;
                }
                return 0x1b;
            }
            if (seq[2] == ';') {
                char mod, fin;
                if (!esc_byte(&mod)) return 0x1b;
                if (!esc_byte(&fin)) return 0x1b;
                if (mod == '5') {          /* Ctrl */
                    if (fin == 'H') return KEY_FILE_TOP;
                    if (fin == 'F') return KEY_FILE_END;
                    if (fin == 'D') return KEY_WORD_LEFT;
                    if (fin == 'C') return KEY_WORD_RIGHT;
                }
                return 0x1b;
            }
            return 0x1b;
        }
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
        return 0x1b;
    }
    if (seq[0] == 'O') {                   /* vt100 application mode */
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
    }
    return 0x1b;
}

/* ── The prompt line ─────────────────────────────────────────────
 * Enter accepts, ESC or Ctrl-C gives up. It edits bytes but erases
 * characters, so backspacing over Hangul takes the whole character. */
static bool prompt(const char *label, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';

    for (;;) {
        move_to(rows, 1);
        outs("\x1b[K");
        outs(label);
        out(buf, len);
        outs("\x1b[?25h");
        flush_out();

        int k = read_key();
        if (k < 0 || k == 0x1b || k == 3 || k == 17) { buf[0] = '\0'; return false; }
        if (k == '\r' || k == '\n')
            return true;
        if (k == 127 || k == 8) {
            if (len) len = utf8_prev(buf, len);
            buf[len] = '\0';
            continue;
        }
        if (k >= 1000)
            continue;                      /* arrows here would need a second cursor */
        if (k == '\t' || (k >= 32 && k < 127)) {
            if (len + 1 < size) { buf[len++] = (char)k; buf[len] = '\0'; }
        } else if (k >= 0xC0 && k < 0x100) {
            int need = utf8_seq_len((unsigned char)k) - 1;
            if (len + (size_t)need + 1 < size) {
                buf[len++] = (char)k;
                for (int i = 0; i < need; i++) {
                    char b;
                    if (lp_read(STDIN_FILENO, &b, 1) != 1)
                        break;
                    buf[len++] = b;
                }
                buf[len] = '\0';
            }
        }
    }
}

/* Ask a question and take one key, leaving the cursor where the text is
 * so you can see what you are answering about. */
static int ask(const char *msg)
{
    move_to(rows, 1);
    outs("\x1b[K");
    outs(msg);
    place_cursor();
    flush_out();
    int k = read_key();
    return k < 0 ? 'q' : k;
}

/* ── Editing ───────────────────────────────────────────────────── */

/* Every key that changes the text comes through here first. */
static bool may_edit(void)
{
    if (!readonly)
        return true;
    set_status("read-only - Ctrl-O saves it under another name");
    return false;
}

static void insert_char(char ch)
{
    line_t *l = &lines[cy];
    if (!line_reserve(l, l->len + 1)) {
        set_status("out of memory");
        return;
    }
    memmove(l->text + cx + 1, l->text + cx, l->len - (size_t)cx + 1);
    l->text[cx] = ch;
    l->len++;
    cx++;
    modified = true;
    dirty = DIRTY_LINE;
}

/* Enter. The new line starts with the same leading whitespace as this
 * one, which is the whole of auto-indent: in C, in shell and in the
 * config files here, the indent of the next line is nearly always the
 * indent of the last one. */
static void split_line(void)
{
    if (nlines + 1 > MAX_LINES) {
        set_status("too many lines");
        return;
    }
    line_t *cur = &lines[cy];

    int indent = 0;
    while (indent < cx && (cur->text[indent] == ' ' || cur->text[indent] == '\t'))
        indent++;

    size_t tail = cur->len - (size_t)cx;
    size_t need = (size_t)cx + 1 + (size_t)indent + tail;
    char  *blob = malloc(need + 1);
    if (!blob) {
        set_status("out of memory");
        return;
    }
    memcpy(blob, cur->text, (size_t)cx);
    blob[cx] = '\n';
    memcpy(blob + cx + 1, cur->text, (size_t)indent);
    memcpy(blob + cx + 1 + indent, cur->text + cx, tail);
    blob[need] = '\0';

    record_change(cy, 1, 2);
    lines_splice(cy, 1, blob, need, 2);
    free(blob);

    cy++;
    cx = indent;
    modified = true;
    dirty = DIRTY_ALL;
}

/* Join line `at` and the one after it. */
static void join_lines(int at)
{
    if (at < 0 || at + 1 >= nlines)
        return;
    size_t need = lines[at].len + lines[at + 1].len;
    char  *blob = malloc(need + 1);
    if (!blob) {
        set_status("out of memory");
        return;
    }
    memcpy(blob, lines[at].text, lines[at].len);
    memcpy(blob + lines[at].len, lines[at + 1].text, lines[at + 1].len);
    blob[need] = '\0';

    int at_col = (int)lines[at].len;
    record_change(at, 2, 1);
    lines_splice(at, 2, blob, need, 1);
    free(blob);

    cy = at;
    cx = at_col;
    modified = true;
    dirty = DIRTY_ALL;
}

/* Erase the character before the cursor. At the start of a line, join
 * it onto the line above. */
static void backspace(void)
{
    if (cx > 0) {
        line_t *l = &lines[cy];
        /* Erase a character, not a byte. Erasing one byte of Hangul
         * would leave the other two behind as a broken character. */
        int start = (int)utf8_prev(l->text, (size_t)cx);
        record_typing();
        memmove(l->text + start, l->text + cx, l->len - (size_t)cx + 1);
        l->len -= (size_t)(cx - start);
        cx = start;
        modified = true;
        dirty = DIRTY_LINE;
        return;
    }
    if (cy > 0)
        join_lines(cy - 1);
}

static void delete_forward(void)
{
    line_t *l = &lines[cy];
    if (cx < (int)l->len) {
        int next = (int)utf8_next(l->text, l->len, (size_t)cx);
        record_typing();
        memmove(l->text + cx, l->text + next, l->len - (size_t)next + 1);
        l->len -= (size_t)(next - cx);
        modified = true;
        dirty = DIRTY_LINE;
        return;
    }
    join_lines(cy);
}

/* ── The clipboard ─────────────────────────────────────────────── */

static char  *clip    = NULL;
static size_t cliplen = 0;

static bool clip_set(const char *text, size_t len)
{
    if (len > CLIP_MAX) {
        set_status("that is more than the clipboard holds (256k)");
        return false;
    }
    char *p = malloc(len + 1);
    if (!p) { set_status("out of memory"); return false; }
    memcpy(p, text, len);
    p[len] = '\0';
    free(clip);
    clip    = p;
    cliplen = len;
    return true;
}

/* A cut line keeps its line break, so pasting it puts a whole line back.
 * A cut region does not, so pasting it drops into the middle of a line. */
static bool copy_selection(void)
{
    int y0, x0, y1, x1;
    if (!sel_bounds(&y0, &x0, &y1, &x1)) {
        set_status("nothing selected - Ctrl-A starts a selection");
        return false;
    }

    size_t total = 0;
    for (int i = y0; i <= y1; i++) {
        int a = (i == y0) ? x0 : 0;
        int b = (i == y1) ? x1 : (int)lines[i].len;
        total += (size_t)(b - a) + 1;
    }
    if (total > CLIP_MAX) {
        set_status("that is more than the clipboard holds (256k)");
        return false;
    }

    char *buf = malloc(total);
    if (!buf) { set_status("out of memory"); return false; }

    size_t off = 0;
    for (int i = y0; i <= y1; i++) {
        int a = (i == y0) ? x0 : 0;
        int b = (i == y1) ? x1 : (int)lines[i].len;
        memcpy(buf + off, lines[i].text + a, (size_t)(b - a));
        off += (size_t)(b - a);
        if (i < y1)
            buf[off++] = '\n';
    }
    bool ok = clip_set(buf, off);
    free(buf);
    return ok;
}

static void delete_selection(void)
{
    int y0, x0, y1, x1;
    if (!sel_bounds(&y0, &x0, &y1, &x1))
        return;

    size_t need = (size_t)x0 + (lines[y1].len - (size_t)x1);
    char  *blob = malloc(need + 1);
    if (!blob) { set_status("out of memory"); return; }
    memcpy(blob, lines[y0].text, (size_t)x0);
    memcpy(blob + x0, lines[y1].text + x1, lines[y1].len - (size_t)x1);
    blob[need] = '\0';

    cy = y0;
    cx = x0;
    record_change(y0, y1 - y0 + 1, 1);
    lines_splice(y0, y1 - y0 + 1, blob, need, 1);
    free(blob);

    mark_on = false;
    modified = true;
    dirty = DIRTY_ALL;
}

static void cut_line(void)
{
    char nl[2] = { '\n', '\0' };
    size_t len = lines[cy].len;
    char  *buf = malloc(len + 2);
    if (buf) {
        memcpy(buf, lines[cy].text, len);
        buf[len] = nl[0];
        clip_set(buf, len + 1);
        free(buf);
    }

    if (nlines == 1) {
        record_change(0, 1, 1);
        lines_splice(0, 1, "", 0, 1);
    } else {
        record_change(cy, 1, 0);
        lines_splice(cy, 1, NULL, 0, 0);
    }
    if (cy >= nlines) cy = nlines - 1;
    cx = 0;
    clamp_cx();
    modified = true;
    dirty = DIRTY_ALL;
}

static void paste(void)
{
    if (!clip || !cliplen) {
        set_status("the clipboard is empty - Ctrl-X or Ctrl-B fills it");
        return;
    }

    int    breaks = 0;
    size_t last   = 0;              /* start of the last line in the clip */
    for (size_t i = 0; i < cliplen; i++)
        if (clip[i] == '\n') { breaks++; last = i + 1; }

    if (nlines + breaks > MAX_LINES) {
        set_status("too many lines");
        return;
    }

    line_t *l    = &lines[cy];
    size_t  need = (size_t)cx + cliplen + (l->len - (size_t)cx);
    char   *blob = malloc(need + 1);
    if (!blob) { set_status("out of memory"); return; }
    memcpy(blob, l->text, (size_t)cx);
    memcpy(blob + cx, clip, cliplen);
    memcpy(blob + cx + cliplen, l->text + cx, l->len - (size_t)cx);
    blob[need] = '\0';

    record_change(cy, 1, 1 + breaks);
    lines_splice(cy, 1, blob, need, 1 + breaks);
    free(blob);

    if (breaks) {
        cy += breaks;
        cx  = (int)(cliplen - last);
    } else {
        cx += (int)cliplen;
    }
    modified = true;
    dirty = DIRTY_ALL;
}

/* ── Moving by words ───────────────────────────────────────────── */

static bool is_word(unsigned char c)
{
    return c == '_' || (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static void word_left(void)
{
    if (cx == 0) {
        if (cy > 0) { cy--; cx = (int)lines[cy].len; }
        return;
    }
    const line_t *l = &lines[cy];
    while (cx > 0 && !is_word((unsigned char)l->text[cx - 1])) cx--;
    while (cx > 0 &&  is_word((unsigned char)l->text[cx - 1])) cx--;
}

static void word_right(void)
{
    const line_t *l = &lines[cy];
    if (cx >= (int)l->len) {
        if (cy < nlines - 1) { cy++; cx = 0; }
        return;
    }
    while (cx < (int)l->len &&  is_word((unsigned char)l->text[cx])) cx++;
    while (cx < (int)l->len && !is_word((unsigned char)l->text[cx])) cx++;
}

/* ── Search and replace ────────────────────────────────────────── */

static char last_pat[PAT_MAX];
static char last_rep[PAT_MAX];
static int  last_dir = 1;

static int find_fwd(const char *hay, size_t len, const char *pat,
                    size_t plen, int from)
{
    if (plen == 0 || plen > len || from < 0)
        return -1;
    for (size_t i = (size_t)from; i + plen <= len; i++)
        if (memcmp(hay + i, pat, plen) == 0)
            return (int)i;
    return -1;
}

static int find_back(const char *hay, size_t len, const char *pat,
                     size_t plen, int before)
{
    if (plen == 0 || plen > len)
        return -1;
    if (before > (int)(len - plen))
        before = (int)(len - plen);
    for (int i = before; i >= 0; i--)
        if (memcmp(hay + i, pat, plen) == 0)
            return i;
    return -1;
}

/* Search from just past the cursor, wrapping round the end of the file
 * back to where it started. */
static bool search(const char *pat, int dir)
{
    size_t plen = strlen(pat);
    if (!plen)
        return false;

    int sy = cy;
    int sx = (dir > 0) ? cx + 1 : cx - 1;

    for (int step = 0; step <= nlines; step++) {
        int y = (dir > 0) ? sy + step : sy - step;
        while (y < 0)        y += nlines;
        while (y >= nlines)  y -= nlines;

        int at;
        if (dir > 0)
            at = find_fwd(lines[y].text, lines[y].len, pat, plen,
                          step == 0 ? sx : 0);
        else
            at = find_back(lines[y].text, lines[y].len, pat, plen,
                           step == 0 ? sx : (int)lines[y].len);
        if (at >= 0) {
            cy = y;
            cx = at;
            dirty = DIRTY_ALL;
            return true;
        }
    }
    return false;
}

static void search_prompt(int dir)
{
    char pat[PAT_MAX];
    if (!prompt(dir > 0 ? "search: " : "search back: ", pat, sizeof(pat)))
        return;
    if (!pat[0]) {
        if (!last_pat[0]) return;
        strlcpy(pat, last_pat, sizeof(pat));
    }
    strlcpy(last_pat, pat, sizeof(last_pat));
    last_dir = dir;

    if (!search(pat, dir)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "not found: %s", pat);
        set_status(msg);
    }
}

static void search_again(void)
{
    if (!last_pat[0]) { set_status("no search to repeat - Ctrl-W starts one"); return; }
    if (!search(last_pat, last_dir)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "not found: %s", last_pat);
        set_status(msg);
    }
}

/* Replace every occurrence in one line, starting at byte `from`.
 * Records nothing: the callers record the whole range at once. */
static int line_replace(int idx, int from, const char *pat, size_t plen,
                        const char *rep, size_t rlen)
{
    line_t *l = &lines[idx];
    int n = 0, at = from;
    for (;;) {
        at = find_fwd(l->text, l->len, pat, plen, at);
        if (at < 0)
            break;
        if (!line_reserve(l, l->len - plen + rlen))
            break;
        memmove(l->text + at + rlen, l->text + at + plen,
                l->len - (size_t)at - plen + 1);
        memcpy(l->text + at, rep, rlen);
        l->len = l->len - plen + rlen;
        at += (int)rlen;
        n++;
    }
    return n;
}

/* Everything from the cursor to the end of the file, as one undo step.
 * A replacement never changes how many lines there are, so the record is
 * just the text of the lines that hold a match - which is why this is
 * one record and not one per occurrence. */
static int replace_rest(const char *pat, size_t plen, const char *rep, size_t rlen)
{
    int first = -1, last = -1;
    for (int i = cy; i < nlines; i++) {
        int from = (i == cy) ? cx : 0;
        if (find_fwd(lines[i].text, lines[i].len, pat, plen, from) >= 0) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0)
        return 0;

    record_change(first, last - first + 1, last - first + 1);

    int n = 0;
    for (int i = first; i <= last; i++)
        n += line_replace(i, (i == cy) ? cx : 0, pat, plen, rep, rlen);

    modified = true;
    dirty = DIRTY_ALL;
    return n;
}

static void replace_prompt(void)
{
    if (!may_edit())
        return;

    char pat[PAT_MAX], rep[PAT_MAX];
    if (!prompt("replace: ", pat, sizeof(pat)) || !pat[0])
        return;
    if (!prompt("with: ", rep, sizeof(rep)))
        return;
    strlcpy(last_pat, pat, sizeof(last_pat));
    strlcpy(last_rep, rep, sizeof(last_rep));

    size_t plen = strlen(pat), rlen = strlen(rep);
    int    done = 0;

    /* Start from the top of the file, not from wherever the cursor
     * happened to be: "replace" that silently skipped the first half of
     * the file is the kind of help nobody wants. */
    cy = 0;
    cx = 0;

    for (;;) {
        int at = find_fwd(lines[cy].text, lines[cy].len, pat, plen, cx);
        if (at < 0) {
            /* No more on this line - go looking on the next ones. */
            int found = -1;
            for (int i = cy + 1; i < nlines && found < 0; i++)
                if (find_fwd(lines[i].text, lines[i].len, pat, plen, 0) >= 0)
                    found = i;
            if (found < 0)
                break;
            cy = found;
            cx = 0;
            continue;
        }

        cx = at;
        dirty = DIRTY_ALL;
        refresh();

        int k = ask("replace this one?  y  n  a=all the rest  q=stop ");
        if (k == 'q' || k == 0x1b || k == 3 || k == 17)
            break;
        if (k == 'a') {
            done += replace_rest(pat, plen, rep, rlen);
            break;
        }
        if (k == 'y') {
            /* Room first: a replacement longer than what it replaces
             * would otherwise run off the end of the line. */
            if (!line_reserve(&lines[cy], lines[cy].len - plen + rlen)) {
                set_status("out of memory - nothing more was replaced");
                break;
            }
            record_change(cy, 1, 1);
            line_t *l = &lines[cy];
            memmove(l->text + at + rlen, l->text + at + plen,
                    l->len - (size_t)at - plen + 1);
            memcpy(l->text + at, rep, rlen);
            l->len = l->len - plen + rlen;

            cx = at + (int)rlen;
            done++;
            modified = true;
        } else {
            cx = at + 1;
        }
    }

    /* Replacing everything in a large file makes an undo record the
     * history cannot hold, and record_change then throws it away. Say
     * that here, or the count would quietly paper over it. */
    char msg[160];
    snprintf(msg, sizeof(msg), "%d replaced%s", done,
             (done && !undo_s.count) ? " - too big to undo" : "");
    set_status(msg);
    clamp_cx();
    dirty = DIRTY_ALL;
}

static void goto_line(void)
{
    char buf[32];
    if (!prompt("go to line: ", buf, sizeof(buf)) || !buf[0])
        return;
    int n = atoi(buf);
    if (n < 1) n = 1;
    if (n > nlines) n = nlines;
    cy = n - 1;
    cx = 0;
    dirty = DIRTY_ALL;
}

static void save_as(void)
{
    char buf[512];
    if (!prompt("save as: ", buf, sizeof(buf)) || !buf[0])
        return;
    if (lp_is_dir(buf)) {
        set_status("that is a directory");
        return;
    }
    if (save_to(buf)) {
        strlcpy(fname, buf, sizeof(fname));
        readonly = !path_writable(fname);
    }
}

/* ── Help ──────────────────────────────────────────────────────── */

static void show_help(void)
{
    outs("\x1b[2J\x1b[H");
    outs("edit - keys\r\n\r\n");
    outs("  arrows  move             Ctrl-Left/Right  word left / right\r\n");
    outs("  Home/End  line ends      PgUp/PgDn        one screen\r\n");
    outs("  Ctrl-Home/End  file ends Enter            split, keeping the indent\r\n");
    outs("  Backspace/Del  erase     Ctrl-K           cut this line\r\n");
    outs("  Ctrl-A  start selecting  Ctrl-B copy  Ctrl-X cut  Ctrl-V paste\r\n");
    outs("  Ctrl-W  search           Ctrl-P  search backward\r\n");
    outs("  Ctrl-N  search again     Ctrl-R  search and replace\r\n");
    outs("  Ctrl-T  go to line       Ctrl-D  line numbers on and off\r\n");
    outs("  Ctrl-Z  undo             Ctrl-Y  redo\r\n");
    outs("  Ctrl-S  save             Ctrl-O  save as\r\n");
    outs("  Ctrl-L  redraw           Ctrl-Q, Ctrl-C  quit\r\n\r\n");
    outs("  Select: Ctrl-A, move to the other end, then Ctrl-X or Ctrl-B.\r\n");
    outs("  Ctrl-A again drops the selection.\r\n\r\n");
    outs("  Saving writes a temporary file in the same directory and\r\n");
    outs("  renames it over. If the write fails the original is untouched.\r\n");
    outs("  Undo remembers the last 64 changes, or 192k of text.\r\n\r\n");
    outs("Press any key...");
    flush_out();
    read_key();
    outs("\x1b[2J");
    dirty = DIRTY_ALL;
}

static void usage(int fd)
{
    dprintf(fd,
            "usage: edit [-n] [-R] <file>\n"
            "  -n  show the line numbers from the start\n"
            "  -R  open read-only\n"
            "Ctrl-G inside the editor lists every key.\n");
}

int main(int argc, char **argv)
{
    bool force_ro = false;
    int  i = 1;

    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-h")) { usage(STDOUT_FILENO); return 0; }
        if (!strcmp(argv[i], "-n")) { show_numbers = true; continue; }
        if (!strcmp(argv[i], "-R")) { force_ro = true; continue; }
        dprintf(STDERR_FILENO, "edit: unknown option %s\n", argv[i]);
        usage(STDERR_FILENO);
        return 2;
    }
    if (i >= argc) {
        usage(STDERR_FILENO);
        return 2;
    }
    strlcpy(fname, argv[i], sizeof(fname));

    if (lp_is_dir(fname)) {
        dprintf(STDERR_FILENO, "edit: %s is a directory\n", fname);
        return 1;
    }

    if (!load_file(fname)) {
        dprintf(STDERR_FILENO, "edit: cannot read %s - out of memory?\n", fname);
        return 1;
    }

    readonly = force_ro || !path_writable(fname);
    if (readonly) {
        char msg[220];
        snprintf(msg, sizeof(msg), "%s  read-only%s",
                 status,
                 force_ro ? " (-R)" : " - it cannot be written here");
        set_status(msg);
    }

    if (lp_term_raw(STDIN_FILENO, &saved_term) < 0) {
        dprintf(STDERR_FILENO,
                "edit: cannot take control of the terminal.\n"
                "      a full-screen editor only works on a terminal.\n");
        return 1;
    }
    lp_term_size(STDOUT_FILENO, &rows, &cols);
    if (rows < 4)  rows = 4;
    if (cols < 20) cols = 20;

    outs("\x1b[2J");                       /* clear the screen */
    refresh();

    int quit_confirm = 0;

    while (running) {
        int k = read_key();
        if (k < 0)
            break;

        status[0] = '\0';                  /* a message lasts one keypress */

        bool was_quit_key = false;
        bool typed        = false;

        switch (k) {
        case KEY_UP:
            if (cy > 0) { cy--; clamp_cx(); }
            break;
        case KEY_DOWN:
            if (cy < nlines - 1) { cy++; clamp_cx(); }
            break;
        case KEY_LEFT:
            if (cx > 0)
                cx = (int)utf8_prev(lines[cy].text, (size_t)cx);
            else if (cy > 0) { cy--; cx = (int)lines[cy].len; }
            break;
        case KEY_RIGHT:
            if (cx < (int)lines[cy].len)
                cx = (int)utf8_next(lines[cy].text, lines[cy].len, (size_t)cx);
            else if (cy < nlines - 1) { cy++; cx = 0; }
            break;
        case KEY_WORD_LEFT:  word_left();  break;
        case KEY_WORD_RIGHT: word_right(); break;
        case KEY_HOME: cx = 0; break;
        case KEY_END:  cx = (int)lines[cy].len; break;
        case KEY_PGUP:
            cy -= rows - 2;
            if (cy < 0) cy = 0;
            clamp_cx();
            break;
        case KEY_PGDN:
            cy += rows - 2;
            if (cy > nlines - 1) cy = nlines - 1;
            clamp_cx();
            break;
        case KEY_FILE_TOP: cy = 0; cx = 0; break;
        case KEY_FILE_END: cy = nlines - 1; cx = (int)lines[cy].len; break;

        /* Erasing counts as typing for the undo history: a run of
         * backspaces on one line is one step to take back, the same as
         * the run of characters that put them there. */
        case KEY_DEL:
            if (may_edit()) {
                if (mark_on) delete_selection();
                else       { delete_forward(); typed = true; }
            }
            break;

        case 127:                          /* Backspace */
        case 8:
            if (may_edit()) {
                if (mark_on) delete_selection();
                else       { backspace(); typed = true; }
            }
            break;

        case '\r':                         /* Enter; ICRNL is off, so it is CR */
        case '\n':
            if (may_edit()) {
                if (mark_on) delete_selection();
                split_line();
            }
            break;

        case 1:                            /* Ctrl-A: the selection anchor */
            if (mark_on) {
                mark_on = false;
                set_status("selection dropped");
            } else {
                mark_on = true;
                my = cy;
                mx = cx;
                set_status("selecting - move, then Ctrl-X cut, Ctrl-B copy");
            }
            dirty = DIRTY_ALL;
            break;

        case 2:                            /* Ctrl-B: copy */
            if (copy_selection()) {
                mark_on = false;
                set_status("copied");
                dirty = DIRTY_ALL;
            }
            break;

        case 24:                           /* Ctrl-X: cut */
            if (may_edit()) {
                if (mark_on) {
                    if (copy_selection())
                        delete_selection();
                } else {
                    cut_line();
                }
            }
            break;

        case 22:                           /* Ctrl-V: paste */
            if (may_edit()) {
                if (mark_on) delete_selection();
                paste();
            }
            break;

        case 11:                           /* Ctrl-K: cut this line */
            if (may_edit())
                cut_line();
            break;

        case 26: if (may_edit()) do_undo(); break;   /* Ctrl-Z */
        case 25: if (may_edit()) do_redo(); break;   /* Ctrl-Y */

        case 23: search_prompt(1);  break;           /* Ctrl-W */
        case 16: search_prompt(-1); break;           /* Ctrl-P */
        case 14: search_again();    break;           /* Ctrl-N */
        case 18: replace_prompt();  break;           /* Ctrl-R */
        case 20: goto_line();       break;           /* Ctrl-T */

        case 4:                            /* Ctrl-D: line numbers */
            show_numbers = !show_numbers;
            left = 0;
            outs("\x1b[2J");
            dirty = DIRTY_ALL;
            break;

        case 19:                           /* Ctrl-S */
            if (readonly)
                set_status("read-only - Ctrl-O saves it under another name");
            else
                save_to(fname);
            break;

        case 15: save_as(); break;         /* Ctrl-O */

        case 7:                            /* Ctrl-G */
            show_help();
            break;

        case 17:                           /* Ctrl-Q */
        case 3:                            /* Ctrl-C - the habit */
            was_quit_key = true;
            if (modified && quit_confirm == 0) {
                set_status("unsaved changes. Ctrl-Q again to quit anyway, "
                           "Ctrl-S to save.");
                quit_confirm = 1;
            } else {
                running = false;
            }
            break;

        case 12:                           /* Ctrl-L: redraw the screen */
            lp_term_size(STDOUT_FILENO, &rows, &cols);
            outs("\x1b[2J");
            dirty = DIRTY_ALL;
            break;

        default:
            if (k == '\t' || (k >= 32 && k < 127)) {
                if (may_edit()) {
                    if (mark_on) delete_selection();
                    record_typing();
                    insert_char((char)k);
                    typed = true;
                }
            } else if (k >= 0xC0 && k < 0x100) {
                /* A UTF-8 lead byte. Read the rest right away and insert
                 * the character as a whole. Inserting byte by byte and
                 * redrawing between would flash a half-formed glyph. */
                if (may_edit()) {
                    if (mark_on) delete_selection();
                    record_typing();
                    int need = utf8_seq_len((unsigned char)k) - 1;
                    insert_char((char)k);
                    for (int j = 0; j < need; j++) {
                        char b;
                        if (lp_read(STDIN_FILENO, &b, 1) != 1)
                            break;
                        insert_char(b);
                    }
                    typed = true;
                }
            }
            /* An orphaned continuation byte, 0x80-0xBF, only makes
             * garbage. Drop it. */
            break;
        }

        if (!typed)
            typing_line = -1;              /* the next keystroke starts a new undo step */
        if (!was_quit_key)
            quit_confirm = 0;

        if (running)
            refresh();
    }

    /* Always put the terminal back, or the shell will look hung. */
    lp_term_restore(STDIN_FILENO, &saved_term);
    outs("\x1b[2J\x1b[H");
    flush_out();
    return 0;
}
