/* httpd - serve static files over HTTP.
 *
 *   httpd                        serve /data/www on port 8080
 *   httpd -p 80 -d /srv/web      another port, another directory
 *   httpd -f                     stay in the foreground
 *   httpd -l /data/log/httpd.log where the request log goes
 *   httpd -i 127.0.0.1           bind one address instead of all of them
 *
 * ── Why this exists ──
 * A board sitting on a shelf for months has things worth looking at -
 * the status JSON beacon writes, the log logd keeps, a page saying what
 * this machine is. Until now the only way to read any of it was to log
 * in over SSH, which means having a key, a client and a keyboard. A URL
 * works from a phone.
 *
 * It serves files. There is no CGI, no scripting, no upload, no PUT and
 * no DELETE. Every one of those is a way to turn "somebody can read my
 * status page" into "somebody owns my board", and none of them is worth
 * that on a machine nobody is watching.
 *
 * ── It is not started by default, and should not be ──
 * There is no line for this in /etc/services. A web server listening on
 * every board whether or not anyone asked for one is a port open on a
 * machine in somebody's house, forever, for no reason. Turn it on
 * deliberately:
 *
 *   service add httpd -f -l /data/log/httpd.log
 *
 * and init will supervise it from then on. Two details in that line:
 * -f, because a supervised service must stay in the foreground - init
 * reads a process that forks and lets the parent exit as an instant
 * death and spends its life restarting it - and -l, because with -f the
 * request log goes to stdout, which on a supervised service means the
 * console. One line per request on the console is fine for a minute and
 * unbearable for a week.
 *
 * ── One process per connection ──
 * The alternative was a single process polling every socket. That is
 * how the big servers do it, and it is the wrong shape here.
 *
 * This board has one core and 512MB. A poll loop means every connection
 * is a state machine - half a request line read, headers half parsed,
 * a file half sent - held in a structure the code has to carry between
 * events, and every one of those partial states is somewhere a bug can
 * hide. Those bugs are reachable from the network. Meanwhile the thing
 * a poll loop buys, tens of thousands of connections, is not something
 * a Zero 2 W will ever be asked for.
 *
 * fork gives each connection a private copy of a straight-line program:
 * read a request, send a file, exit. Nothing is shared, so nothing
 * leaks between clients, and a child that dies badly takes only its own
 * connection with it. The cost is a process, and on Linux with
 * copy-on-write that is a page table and little else for a program this
 * size - the text is shared with the parent and never written.
 *
 * What fork does not give you is a bound, so there is one: MAX_CONN
 * children, and connection MAX_CONN+1 gets a 503 and a Retry-After
 * rather than a fork that fails somewhere deep in the kernel. The
 * parent's own loop uses poll on the listening socket with a timeout,
 * for one reason: it needs to wake up regularly to reap children that
 * have finished, or the count would only fall when the next client
 * happened to arrive.
 *
 * ── Slowloris ──
 * The attack on a server this size is not a flood, it is patience: open
 * MAX_CONN connections, send one header byte a minute, and every slot
 * is gone with almost no traffic. Three things stop it. Every socket
 * has a receive timeout, so a connection that says nothing is closed.
 * The request line, each header, the header count and the total header
 * bytes are all capped, so a header that never ends is refused rather
 * than buffered. And keep-alive has its own shorter idle timeout, so a
 * browser that opens six connections and holds them gives five back.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define DEF_PORT      8080
#define DEF_ROOT      "/data/www"
#define DEF_LOG       "/data/log/httpd.log"

/* Limits. Every one of these exists because the other side of the
 * socket is a stranger with an opinion about how much of this board's
 * memory it would like. */
#define MAX_CONN        32      /* children alive at once */
#define REQ_LINE_MAX  2048      /* "GET <target> HTTP/1.1" */
#define HDR_LINE_MAX  1024      /* one header */
#define MAX_HEADERS     48
#define HDR_TOTAL_MAX 8192      /* all headers together */
#define PATH_MAX      1024
#define COPY_BUF      4096      /* per child, so keep it small */
#define LIST_BUF     16384      /* a generated directory listing */
#define MAX_LIST       128      /* names in one listing */
#define NAME_MAX        64

#define FIRST_TIMEOUT   15      /* seconds to wait for the first request */
#define IDLE_TIMEOUT     5      /* ... and for the next one on keep-alive */
#define SEND_TIMEOUT    20      /* a client that stops reading */
#define MAX_REQUESTS    64      /* requests served on one connection */

/* SIGPIPE is not in unistd.h because nothing here had ever written to a
 * socket the far end had closed. A server does that constantly - the
 * browser tab shuts halfway through a file - and the default action is
 * to kill the process, which would lose the log line for the request. */
#define SIGPIPE 13

/* EAGAIN is what a socket read returns when the receive timeout fires.
 * Telling that apart from a real error is the difference between "408,
 * you took too long" and closing without a word. */
#define LP_EAGAIN 11

static char  g_root[PATH_MAX] = DEF_ROOT;
static int   g_logfd = STDOUT_FILENO;

/* ── content types ────────────────────────────────────────────────
 *
 * The two dozen extensions that actually turn up. Everything else is
 * application/octet-stream, which makes a browser download it rather
 * than guess - the safe direction, since guessing is how a .txt full of
 * markup becomes a script running on the page.
 *
 * charset=utf-8 on the text types is not decoration: without it a
 * browser falls back to a locale-dependent guess, and Hangul in a page
 * served off this board comes out as mojibake on some machines and not
 * others. */
static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return "application/octet-stream";
    dot++;

    static const struct { const char *ext, *type; } types[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "svg",  "image/svg+xml" },
        { "webp", "image/webp" },
        { "ico",  "image/vnd.microsoft.icon" },
        { "txt",  "text/plain; charset=utf-8" },
        { "md",   "text/markdown; charset=utf-8" },
        { "xml",  "application/xml" },
        { "pdf",  "application/pdf" },
        { "zip",  "application/zip" },
        { "tar",  "application/x-tar" },
        { "gz",   "application/gzip" },
        { "wasm", "application/wasm" },
        { "woff", "font/woff" },
        { "woff2","font/woff2" },
        { "mp4",  "video/mp4" },
        { "webm", "video/webm" },
        { "mp3",  "audio/mpeg" },
        { "wav",  "audio/wav" },
        { "csv",  "text/csv; charset=utf-8" },
    };
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcmp(dot, types[i].ext) == 0)
            return types[i].type;
    return "application/octet-stream";
}

/* ── dates ────────────────────────────────────────────────────────
 *
 * HTTP dates are always UTC and always English, whatever the machine's
 * idea of a locale is, which is convenient here because there is no
 * locale at all. `out` needs 32 bytes. */
static const char *const WDAY[7] =
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON[12] =
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void http_date(s64 t, char *out, size_t n)
{
    lp_tm_t tm;
    lp_gmtime(t, &tm);
    if (tm.mon < 1 || tm.mon > 12) tm.mon = 1;
    if (tm.wday < 0 || tm.wday > 6) tm.wday = 0;
    snprintf(out, n, "%s, %02d %s %d %02d:%02d:%02d GMT",
             WDAY[tm.wday], tm.day, MON[tm.mon - 1], tm.year,
             tm.hour, tm.min, tm.sec);
}

static bool digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

static int num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}

/* "Sun, 06 Nov 1994 08:49:37 GMT" -> unix seconds. false if it is not
 * that shape.
 *
 * RFC 9110 also lists the RFC 850 form and asctime's, and says a server
 * must accept them. Nothing has sent either since the 1990s, and the
 * cost of getting it wrong is small in the right direction: an
 * unparsed If-Modified-Since means the file is sent again, not that
 * something stale is served. So: one format, and a comment saying so. */
static bool http_date_parse(const char *s, s64 *out)
{
    while (*s == ' ') s++;
    if (strlen(s) < 25)
        return false;
    if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' ||
        s[16] != ' ' || s[19] != ':' || s[22] != ':')
        return false;
    if (!digits(s + 5, 2) || !digits(s + 12, 4) || !digits(s + 17, 2) ||
        !digits(s + 20, 2) || !digits(s + 23, 2))
        return false;

    int mon = 0;
    for (int i = 0; i < 12; i++)
        if (strncmp(s + 8, MON[i], 3) == 0) { mon = i + 1; break; }
    if (!mon)
        return false;

    lp_tm_t tm;
    memset(&tm, 0, sizeof tm);
    tm.day  = num(s + 5, 2);
    tm.mon  = mon;
    tm.year = num(s + 12, 4);
    tm.hour = num(s + 17, 2);
    tm.min  = num(s + 20, 2);
    tm.sec  = num(s + 23, 2);
    *out = lp_timegm(&tm);
    return true;
}

/* ── the log ──────────────────────────────────────────────────────
 *
 * One line per request:
 *   2026-09-05T12:34:56Z 192.168.1.5 GET /index.html 200 4213
 *
 * Written with a single write() of well under a page, which is what
 * makes it safe for thirty-odd children to share one O_APPEND file
 * descriptor without a lock: the kernel does not interleave those. Two
 * writes per line would produce lines from two clients spliced
 * together, which is the kind of log you cannot trust afterwards. */
static void log_request(const char *addr, const char *method,
                        const char *path, int status, u64 bytes)
{
    if (g_logfd < 0)
        return;

    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);

    /* The path is trimmed here rather than with a "%.300s" precision,
     * because this printf has no precision: it copies "%.300s" out
     * literally and then every argument after it lands in the wrong
     * conversion. The log said "GET %.400s -765559792 200" until this
     * was written the long way. */
    char shortpath[301], shortmethod[24];
    strlcpy(shortpath, path[0] ? path : "-", sizeof shortpath);
    /* The method comes off the request line, so it is whatever the
     * client felt like sending - up to a couple of thousand bytes of
     * it. Trimmed for the same reason as the path: `line` below is
     * sized so that the newline at the end can never be the part that
     * gets cut, and two log lines can never run together. */
    strlcpy(shortmethod, method[0] ? method : "-", sizeof shortmethod);

    char line[420];
    int n = snprintf(line, sizeof line,
                     "%04d-%02d-%02dT%02d:%02d:%02dZ %s %s %s %d %llu\n",
                     tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec,
                     addr, shortmethod, shortpath, status,
                     (unsigned long long)bytes);
    if (n > 0) {
        if (n > (int)sizeof line - 1)
            n = (int)sizeof line - 1;   /* snprintf returns what it wanted */
        lp_write(g_logfd, line, (size_t)n);
    }
}

/* ── the connection ───────────────────────────────────────────────
 *
 * A small read buffer, because keep-alive means a read can return the
 * end of one request and the start of the next, and the leftover has to
 * be kept. */
typedef struct {
    int  fd;
    char buf[1024];
    int  len, pos;
} conn_t;

#define RD_EOF    (-1)
#define RD_ERR    (-2)
#define RD_TMOUT  (-3)
#define RD_LONG   (-4)
#define RD_NUL    (-5)

static int conn_getline(conn_t *c, char *out, int max)
{
    int n = 0;
    for (;;) {
        if (c->pos >= c->len) {
            long got = lp_read(c->fd, c->buf, sizeof c->buf);
            if (got == 0)
                return n > 0 ? RD_EOF : RD_EOF;
            if (got < 0)
                return (-got == LP_EAGAIN) ? RD_TMOUT : RD_ERR;
            c->len = (int)got;
            c->pos = 0;
        }
        char ch = c->buf[c->pos++];
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return n;
        }
        /* A NUL inside the request. Nothing legitimate sends one, and
         * everything downstream of here is C strings - a path that
         * looks like "/ok\0/../../etc/passwd" is checked as one string
         * and opened as another. Refused before decoding, and %00 is
         * refused again after. */
        if (ch == '\0')
            return RD_NUL;
        if (n >= max - 1)
            return RD_LONG;
        out[n++] = ch;
    }
}

static bool conn_write(conn_t *c, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        long w = lp_write(c->fd, p, n);
        if (w <= 0)
            return false;       /* the client left, or stopped reading */
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Header names are case insensitive, and there is no strcasecmp here.
 * `want` is lower case; `name` is whatever the client typed. Matching
 * only the two spellings curl happens to use is the kind of thing that
 * works for a year and then meets a proxy that shouts. */
static bool hdr_is(const char *name, const char *want)
{
    for (; *name && *want; name++, want++) {
        char a = *name;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (a != *want)
            return false;
    }
    return *name == '\0' && *want == '\0';
}

static void set_timeout(int fd, int opt, int seconds)
{
    s64 tv[2] = { seconds, 0 };
    lp_setsockopt(fd, SOL_SOCKET, opt, tv, sizeof tv);
}

/* ── paths ────────────────────────────────────────────────────────
 *
 * The whole of the security of this program is in the next sixty
 * lines, so they are written to be read rather than to be short.
 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Turn a request target into a path under the root, or say no.
 *
 * The order matters and is the point:
 *
 *   1. drop the query string - "?" and everything after it is not a
 *      path and must never reach the filesystem
 *   2. refuse ".." in the raw target, before any decoding
 *   3. decode %XX exactly once - ONCE. Decoding twice is the classic
 *      hole: %252e%252e decodes to %2e%2e and then to "..", and a
 *      server that loops until nothing changes walks straight out of
 *      the root. Here %252e%252e stays the literal name "%2e%2e" and
 *      gets a 404, which is the correct answer.
 *   4. refuse %00 while decoding, and ".." again afterwards
 *   5. collapse "//" and drop "." segments, so what is checked below is
 *      the same string that gets opened
 *
 * ".." is refused wherever it appears, not only as a whole segment. A
 * file really named "a..b" is legal and this will not serve it. That is
 * a name almost nobody has, and refusing it removes an entire class of
 * bug where the code splitting segments and the kernel resolving them
 * disagree about where a segment ends.
 */
static bool clean_path(const char *target, char *out, size_t outsz)
{
    char raw[PATH_MAX], dec[PATH_MAX];

    /* "GET http://board/index.html HTTP/1.1" is legal to send to any
     * server, not just a proxy, and the spec says accept it. No browser
     * does it, but something will, and 403 would be the wrong answer.
     * The host part is thrown away: this server has one document root
     * and does not care what name was used to reach it. */
    if (strncmp(target, "http://", 7) == 0) {
        const char *slash = strchr(target + 7, '/');
        target = slash ? slash : "/";
    }

    size_t n = 0;
    for (const char *p = target; *p && *p != '?' && *p != '#'; p++) {
        if (n >= sizeof raw - 1)
            return false;
        raw[n++] = *p;
    }
    raw[n] = '\0';

    if (raw[0] != '/')
        return false;
    if (strstr(raw, ".."))
        return false;

    size_t d = 0;
    for (size_t i = 0; raw[i]; i++) {
        if (raw[i] == '%') {
            int hi = hexval(raw[i + 1]);
            int lo = hi < 0 ? -1 : hexval(raw[i + 2]);
            if (lo < 0)
                return false;               /* a malformed escape */
            int v = hi * 16 + lo;
            if (v == 0)
                return false;               /* %00 */
            if (v == '/')
                return false;               /* %2f hides a separator */
            if (d >= sizeof dec - 1)
                return false;
            dec[d++] = (char)v;
            i += 2;
        } else {
            if (d >= sizeof dec - 1)
                return false;
            dec[d++] = raw[i];
        }
    }
    dec[d] = '\0';

    if (strstr(dec, ".."))
        return false;

    /* Collapse. The result always starts with '/' and never contains
     * "//", so appending it to the root gives one unambiguous path. */
    size_t o = 0;
    for (size_t i = 0; dec[i]; i++) {
        if (dec[i] == '/' && o > 0 && out[o - 1] == '/')
            continue;
        if (dec[i] == '.' && o > 0 && out[o - 1] == '/' &&
            (dec[i + 1] == '/' || dec[i + 1] == '\0')) {
            if (dec[i + 1] == '/')
                i++;
            continue;
        }
        if (o >= outsz - 1)
            return false;
        out[o++] = dec[i];
    }
    out[o] = '\0';
    if (out[0] != '/')
        return false;
    return true;
}

/* Walk the path one segment at a time from the root, and refuse a
 * symlink anywhere along it.
 *
 * This is what "confirm it is still under the document root" means
 * here. There is no realpath() in this libc, and writing one is exactly
 * the kind of string code that gets a path traversal wrong. Walking
 * instead asks the kernel about every component: if none of them is a
 * symlink, and none of them is "..", then the string cannot name
 * anything outside the root - there is no way out of a directory tree
 * except through one of those two.
 *
 * So no symlink under the document root is served at all, not even one
 * pointing at a file next to it. Telling the safe symlink from the
 * dangerous one means resolving it and comparing prefixes, and that is
 * the code this is avoiding. If a file has to appear in two places,
 * make it a hard link or copy it.
 *
 * Returns 200 when the path is fine and fills in `st`, or the status to
 * send: 403 for a symlink, 404 for a missing or non-directory
 * component. */
static int walk_path(const char *rel, char *full, size_t fullsz,
                     lp_stat_t *st)
{
    strlcpy(full, g_root, fullsz);
    size_t base = strlen(full);
    while (base > 1 && full[base - 1] == '/')
        full[--base] = '\0';

    if (lp_stat(full, st, false) < 0)
        return 404;

    const char *p = rel;
    while (*p == '/') p++;

    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);
        if (seglen == 0)
            break;

        size_t cur = strlen(full);
        if (cur + 1 + seglen >= fullsz)
            return 414;
        full[cur] = '/';
        memcpy(full + cur + 1, seg, seglen);
        full[cur + 1 + seglen] = '\0';

        long rc = lp_stat(full, st, false);
        if (rc < 0) {
            /* EACCES is worth telling apart from ENOENT: "you may not"
             * and "there is nothing there" send whoever is running this
             * to completely different places, and after the privileges
             * are dropped the first one is the likely mistake. */
            return -rc == 13 ? 403 : 404;
        }
        if ((st->mode & LP_S_IFMT) == LP_S_IFLNK)
            return 403;

        while (*p == '/') p++;
        if (*p && (st->mode & LP_S_IFMT) != LP_S_IFDIR)
            return 404;
    }
    return 200;
}

/* ── responses ────────────────────────────────────────────────────*/

static const char *reason_for(int code)
{
    switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

/* Send a complete short response with a one-line HTML body.
 * `extra` is any additional header lines, each ending in CRLF, or NULL. */
static bool send_error(conn_t *c, int code, const char *extra,
                       bool keepalive, const char *method)
{
    char body[256], head[640], date[40];
    const char *reason = reason_for(code);

    int blen = snprintf(body, sizeof body,
                        "<!doctype html><meta charset=\"utf-8\">"
                        "<title>%d %s</title>\n"
                        "<h1>%d %s</h1>\n", code, reason, code, reason);
    http_date(lp_time(), date, sizeof date);

    int hlen = snprintf(head, sizeof head,
                        "HTTP/1.1 %d %s\r\n"
                        "Date: %s\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: %s\r\n"
                        "%s\r\n",
                        code, reason, date, blen,
                        keepalive ? "keep-alive" : "close",
                        extra ? extra : "");
    if (!conn_write(c, head, (size_t)hlen))
        return false;
    if (method && strcmp(method, "HEAD") == 0)
        return true;
    return conn_write(c, body, (size_t)blen);
}

/* ── directory listing ────────────────────────────────────────────*/

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static void html_escape(const char *s, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *s && o + 8 < outsz; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;";  break;
        case '<': rep = "&lt;";   break;
        case '>': rep = "&gt;";   break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}

/* Percent-encode what must not be taken as syntax in an href. Letters,
 * digits and the handful of safe punctuation go through untouched, so a
 * Hangul filename stays readable as UTF-8 in the source of the page and
 * still works when clicked. */
static void url_escape(const char *s, char *out, size_t outsz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s;
         *p && o + 4 < outsz; p++) {
        unsigned char ch = *p;
        bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
                    ch >= 0x80;
        if (safe) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = '\0';
}

static char g_list[LIST_BUF];
static char g_names[MAX_LIST][NAME_MAX];

/* Build the listing page for `dir`, which is shown as `rel`.
 * Returns its length, or -1 if the directory cannot be read. */
static int build_listing(const char *dir, const char *rel)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (fd < 0)
        return -1;

    int count = 0;
    bool truncated = false;
    char dbuf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, dbuf, sizeof dbuf);
        if (got <= 0)
            break;
        for (long off = 0; off < got; ) {
            char *rec = dbuf + off;
            u16 reclen = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (reclen == 0)
                break;
            off += reclen;
            if (name[0] == '.')                 /* including . and .. */
                continue;
            if (strlen(name) >= NAME_MAX)
                continue;
            if (count >= MAX_LIST) { truncated = true; continue; }
            strlcpy(g_names[count++], name, NAME_MAX);
        }
    }
    lp_close((int)fd);

    /* Insertion sort. The list is at most MAX_LIST long and this runs
     * once per directory request, so the quadratic worst case is a few
     * thousand comparisons - cheaper than the code for anything
     * cleverer, and that code would have to be right. */
    for (int i = 1; i < count; i++) {
        char tmp[NAME_MAX];
        strlcpy(tmp, g_names[i], NAME_MAX);
        int j = i - 1;
        while (j >= 0 && strcmp(g_names[j], tmp) > 0) {
            strlcpy(g_names[j + 1], g_names[j], NAME_MAX);
            j--;
        }
        strlcpy(g_names[j + 1], tmp, NAME_MAX);
    }

    char esc[NAME_MAX * 8], enc[NAME_MAX * 4], reldisp[768];
    html_escape(rel, reldisp, sizeof reldisp);

    /* snprintf here returns the length it WANTED to write, not the
     * length it wrote, so every accumulation is clamped. Getting that
     * backwards is how a "%d bytes written" counter walks off the end
     * of the buffer it is supposed to be filling. */
    const int cap = (int)sizeof g_list - 1;
    int len = snprintf(g_list, sizeof g_list,
                       "<!doctype html><meta charset=\"utf-8\">"
                       "<title>%s</title>\n"
                       "<h1>%s</h1>\n<ul>\n", reldisp, reldisp);
    if (len > cap) len = cap;
    if (strcmp(rel, "/") != 0) {
        len += snprintf(g_list + len, (size_t)(cap - len) + 1,
                        "<li><a href=\"../\">../</a></li>\n");
        if (len > cap) len = cap;
    }

    for (int i = 0; i < count; i++) {
        /* Leave room for the longest entry plus the closing tags, so
         * the page always closes itself however many names there were. */
        if (len + NAME_MAX * 13 > cap - 256) {
            truncated = true;
            break;
        }
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, g_names[i]);
        lp_stat_t st;
        bool isdir = lp_stat(full, &st, false) >= 0 &&
                     (st.mode & LP_S_IFMT) == LP_S_IFDIR;

        html_escape(g_names[i], esc, sizeof esc);
        url_escape(g_names[i], enc, sizeof enc);
        len += snprintf(g_list + len, (size_t)(cap - len) + 1,
                        "<li><a href=\"%s%s\">%s%s</a></li>\n",
                        enc, isdir ? "/" : "", esc, isdir ? "/" : "");
        if (len > cap) len = cap;
    }

    len += snprintf(g_list + len, (size_t)(cap - len) + 1,
                    "</ul>\n%s", truncated
                    ? "<p>this listing was cut short - there are more "
                      "files here than it will show.</p>\n" : "");
    if (len > cap) len = cap;
    return len;
}

/* ── ranges ───────────────────────────────────────────────────────
 *
 * One range, which is what a resumed download and a video seek both
 * send. A request for several at once needs a multipart/byteranges
 * body, and the spec is explicit that a server may answer any range
 * request with the whole file instead - so that is what a comma gets.
 *
 * Returns 1 for a usable range, 0 for "ignore this and send it all",
 * -1 for a range past the end of the file. */
static int parse_range(const char *v, u64 size, u64 *start, u64 *end)
{
    while (*v == ' ') v++;
    if (strncmp(v, "bytes=", 6) != 0)
        return 0;
    v += 6;
    if (strchr(v, ','))
        return 0;

    const char *dash = strchr(v, '-');
    if (!dash)
        return 0;

    if (dash == v) {
        /* "-500": the last 500 bytes. */
        if (!digits(dash + 1, 1))
            return 0;
        long want = strtol(dash + 1, NULL, 10);
        if (want < 0)
            return 0;
        if (want == 0 || size == 0)
            return -1;
        *start = (u64)want >= size ? 0 : size - (u64)want;
        *end   = size - 1;
        return 1;
    }

    if (!digits(v, 1))
        return 0;
    long from = strtol(v, NULL, 10);
    if (from < 0)
        return 0;
    *start = (u64)from;
    if (dash[1] == '\0') {
        *end = size ? size - 1 : 0;
    } else if (digits(dash + 1, 1)) {
        long to = strtol(dash + 1, NULL, 10);
        if (to < 0)
            return 0;
        *end = (u64)to;
    } else {
        return 0;
    }

    if (size == 0 || *start >= size)
        return -1;
    if (*end >= size)
        *end = size - 1;
    if (*end < *start)
        return -1;
    return 1;
}

/* ── serving one file ─────────────────────────────────────────────*/

static bool send_file(conn_t *c, const char *full, const lp_stat_t *st,
                      bool head_only, const char *range_hdr,
                      s64 ims, bool keepalive, int *status, u64 *sent)
{
    char date[40], lastmod[40], hdr[768];

    http_date(lp_time(), date, sizeof date);
    http_date(st->mtime, lastmod, sizeof lastmod);

    /* 304 before anything is opened. The point of a conditional request
     * is that the server does no work, and reading a file just to throw
     * it away would give up most of that. */
    if (ims > 0 && st->mtime <= ims) {
        *status = 304;
        *sent = 0;
        int n = snprintf(hdr, sizeof hdr,
                         "HTTP/1.1 304 Not Modified\r\n"
                         "Date: %s\r\n"
                         "Last-Modified: %s\r\n"
                         "Connection: %s\r\n\r\n",
                         date, lastmod, keepalive ? "keep-alive" : "close");
        return conn_write(c, hdr, (size_t)n);
    }

    u64 size = st->size;
    u64 first = 0, last = size ? size - 1 : 0;
    int rc = range_hdr ? parse_range(range_hdr, size, &first, &last) : 0;

    if (rc < 0) {
        *status = 416;
        *sent = 0;
        char extra[80];
        snprintf(extra, sizeof extra, "Content-Range: bytes */%llu\r\n",
                 (unsigned long long)size);
        return send_error(c, 416, extra, keepalive, head_only ? "HEAD" : "GET");
    }

    long fd = lp_open(full, O_RDONLY | O_NOFOLLOW, 0);
    if (fd < 0) {
        /* O_NOFOLLOW is the second line of defence: walk_path already
         * refused a symlink, but between that check and this open the
         * file could have been replaced. Here the open simply fails. */
        *status = 403;
        *sent = 0;
        return send_error(c, 403, NULL, keepalive, head_only ? "HEAD" : "GET");
    }

    u64 length = rc == 1 ? last - first + 1 : size;
    int n;
    if (rc == 1) {
        *status = 206;
        n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Date: %s\r\n"
                     "Last-Modified: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Range: bytes %llu-%llu/%llu\r\n"
                     "Content-Length: %llu\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Connection: %s\r\n\r\n",
                     date, lastmod, content_type(full),
                     (unsigned long long)first, (unsigned long long)last,
                     (unsigned long long)size, (unsigned long long)length,
                     keepalive ? "keep-alive" : "close");
    } else {
        *status = 200;
        n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Date: %s\r\n"
                     "Last-Modified: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Connection: %s\r\n\r\n",
                     date, lastmod, content_type(full),
                     (unsigned long long)length,
                     keepalive ? "keep-alive" : "close");
    }

    *sent = 0;
    if (!conn_write(c, hdr, (size_t)n)) {
        lp_close((int)fd);
        return false;
    }
    if (head_only) {
        lp_close((int)fd);
        return true;
    }

    if (first > 0)
        lp_lseek((int)fd, (off_t)first, SEEK_SET);

    static char copy[COPY_BUF];
    u64 left = length;
    bool ok = true;
    while (left > 0) {
        size_t want = left < sizeof copy ? (size_t)left : sizeof copy;
        long got = lp_read((int)fd, copy, want);
        if (got <= 0) {
            /* The file shrank under us, or the card threw an error.
             * Content-Length has already gone out, so there is nothing
             * honest left to do but close and let the client see a
             * short body rather than a lie about what follows. */
            ok = false;
            break;
        }
        if (!conn_write(c, copy, (size_t)got)) {
            ok = false;
            break;
        }
        *sent += (u64)got;
        left -= (u64)got;
    }
    lp_close((int)fd);
    return ok;
}

/* ── one connection ───────────────────────────────────────────────*/

static void serve(int fd, const char *addr)
{
    conn_t c;
    memset(&c, 0, sizeof c);
    c.fd = fd;

    set_timeout(fd, SO_SNDTIMEO_NEW, SEND_TIMEOUT);

    for (int req = 0; req < MAX_REQUESTS; req++) {
        set_timeout(fd, SO_RCVTIMEO_NEW,
                    req == 0 ? FIRST_TIMEOUT : IDLE_TIMEOUT);

        char line[REQ_LINE_MAX];
        int n = conn_getline(&c, line, sizeof line);
        if (n == RD_EOF || n == RD_ERR)
            return;
        if (n == RD_TMOUT) {
            /* Slowloris ends here: the connection opened and said
             * nothing, or stopped halfway. Say so and go. */
            if (req == 0) {
                send_error(&c, 408, NULL, false, "GET");
                log_request(addr, "-", "-", 408, 0);
            }
            return;
        }
        if (n == RD_LONG) {
            send_error(&c, 414, NULL, false, "GET");
            log_request(addr, "-", "-", 414, 0);
            return;
        }
        if (n == RD_NUL) {
            send_error(&c, 400, NULL, false, "GET");
            log_request(addr, "-", "-", 400, 0);
            return;
        }
        /* A stray empty line before the request. The spec says ignore
         * at least one, and old clients really do send them. It costs
         * one of this connection's MAX_REQUESTS turns, so a client
         * sending nothing but blank lines runs out rather than looping
         * here forever. */
        if (n == 0)
            continue;

        /* "GET /path HTTP/1.1" */
        char *sp1 = strchr(line, ' ');
        if (!sp1) {
            send_error(&c, 400, NULL, false, "GET");
            log_request(addr, "-", "-", 400, 0);
            return;
        }
        *sp1 = '\0';
        char *target = sp1 + 1;
        while (*target == ' ') target++;
        char *sp2 = strchr(target, ' ');
        if (!sp2) {
            send_error(&c, 400, NULL, false, "GET");
            log_request(addr, line, "-", 400, 0);
            return;
        }
        *sp2 = '\0';
        const char *method = line;
        const char *version = sp2 + 1;
        while (*version == ' ') version++;

        bool keepalive;
        if (strcmp(version, "HTTP/1.1") == 0)
            keepalive = true;
        else if (strcmp(version, "HTTP/1.0") == 0)
            keepalive = false;
        else {
            /* Including HTTP/0.9, which had no version on the line at
             * all and no headers to answer with. */
            send_error(&c, 400, NULL, false, "GET");
            log_request(addr, method, target, 400, 0);
            return;
        }

        /* Headers. The caps here are the whole defence against a client
         * that intends to send a header for the rest of the afternoon. */
        set_timeout(fd, SO_RCVTIMEO_NEW, FIRST_TIMEOUT);
        char range[128] = "", ims_raw[64] = "";
        bool have_range = false;
        s64 ims = 0;
        int hdr_count = 0, hdr_bytes = 0;
        /* 0 = fine, -1 = the client vanished, anything else is the
         * status to send before closing. */
        int fail = 0;

        for (;;) {
            char h[HDR_LINE_MAX];
            int hn = conn_getline(&c, h, sizeof h);
            if (hn == 0)
                break;
            if (hn == RD_LONG)  { fail = 431; break; }
            if (hn == RD_TMOUT) { fail = 408; break; }
            if (hn == RD_NUL)   { fail = 400; break; }
            if (hn < 0)         { fail = -1;  break; }

            hdr_bytes += hn;
            if (++hdr_count > MAX_HEADERS || hdr_bytes > HDR_TOTAL_MAX) {
                fail = 431;
                break;
            }

            char *colon = strchr(h, ':');
            if (!colon)
                continue;                   /* not a header; ignore it */
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;

            if (hdr_is(h, "connection")) {
                char v[64];
                strlcpy(v, val, sizeof v);
                for (char *q = v; *q; q++)
                    if (*q >= 'A' && *q <= 'Z')
                        *q = (char)(*q + 32);
                if (strstr(v, "close"))
                    keepalive = false;
            } else if (hdr_is(h, "range")) {
                strlcpy(range, val, sizeof range);
                have_range = true;
            } else if (hdr_is(h, "if-modified-since")) {
                strlcpy(ims_raw, val, sizeof ims_raw);
            } else if (hdr_is(h, "content-length") ||
                       hdr_is(h, "transfer-encoding")) {
                /* A GET with a body. Nothing here reads bodies, so the
                 * bytes would still be in the socket when the next
                 * request was read off it - and the client would get to
                 * choose what that "request" said. That is request
                 * smuggling, and the fix is to not reuse the
                 * connection: answer this one and close. */
                if (!(val[0] == '0' && val[1] == '\0'))
                    keepalive = false;
            }
        }

        if (fail < 0)
            return;
        if (fail > 0) {
            send_error(&c, fail, NULL, false, method);
            log_request(addr, method, target, fail, 0);
            return;
        }

        if (ims_raw[0] && !http_date_parse(ims_raw, &ims))
            ims = 0;

        bool is_head = strcmp(method, "HEAD") == 0;
        if (!is_head && strcmp(method, "GET") != 0) {
            /* Allow is required on a 405 and is the whole answer to
             * "what can I do here then". The connection closes whatever
             * the client asked for: a POST has a body this server never
             * reads, and leaving it in the socket would make the next
             * request whatever the client put after it. */
            send_error(&c, 405, "Allow: GET, HEAD\r\n", false, method);
            log_request(addr, method, target, 405, 0);
            return;
        }

        char rel[PATH_MAX];
        if (!clean_path(target, rel, sizeof rel)) {
            send_error(&c, 403, NULL, keepalive, method);
            log_request(addr, method, target, 403, 0);
            if (!keepalive) return;
            continue;
        }

        char full[PATH_MAX];
        lp_stat_t st;
        int code = walk_path(rel, full, sizeof full, &st);
        if (code != 200) {
            send_error(&c, code, NULL, keepalive, method);
            log_request(addr, method, rel, code, 0);
            if (!keepalive) return;
            continue;
        }

        int status = 200;
        u64 sent = 0;
        bool alive = true;

        if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
            size_t rl = strlen(rel);
            if (rl == 0 || rel[rl - 1] != '/') {
                /* A directory without the trailing slash: every
                 * relative link on the page it is about to serve would
                 * resolve one level too high. Redirect rather than
                 * serve something whose links are all broken. */
                char loc[PATH_MAX + 64], enc[PATH_MAX * 3];
                url_escape(rel, enc, sizeof enc);
                /* url_escape encodes '/', which is right inside a name
                 * and wrong for the path we just built, so put them
                 * back. */
                for (char *q = enc; *q; q++)
                    if (q[0] == '%' && q[1] == '2' &&
                        (q[2] == 'F' || q[2] == 'f')) {
                        *q = '/';
                        memmove(q + 1, q + 3, strlen(q + 3) + 1);
                    }
                snprintf(loc, sizeof loc, "Location: %s/\r\n", enc);
                status = 301;
                alive = send_error(&c, 301, loc, keepalive, method);
            } else {
                char index[PATH_MAX];
                lp_stat_t ist;
                /* walk_path never leaves a trailing slash on `full`,
                 * so the separator goes in here. */
                snprintf(index, sizeof index, "%s/index.html", full);
                if (lp_stat(index, &ist, false) >= 0 &&
                    (ist.mode & LP_S_IFMT) == LP_S_IFREG) {
                    alive = send_file(&c, index, &ist, is_head,
                                      have_range ? range : NULL, ims,
                                      keepalive, &status, &sent);
                } else {
                    int len = build_listing(full, rel);
                    if (len < 0) {
                        status = 403;
                        alive = send_error(&c, 403, NULL, keepalive, method);
                    } else {
                        char date[40], hdr[512];
                        http_date(lp_time(), date, sizeof date);
                        int hn = snprintf(hdr, sizeof hdr,
                                "HTTP/1.1 200 OK\r\n"
                                "Date: %s\r\n"
                                "Content-Type: text/html; charset=utf-8\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: %s\r\n\r\n",
                                date, len,
                                keepalive ? "keep-alive" : "close");
                        alive = conn_write(&c, hdr, (size_t)hn);
                        if (alive && !is_head) {
                            alive = conn_write(&c, g_list, (size_t)len);
                            sent = (u64)len;
                        }
                    }
                }
            }
        } else if ((st.mode & LP_S_IFMT) == LP_S_IFREG) {
            alive = send_file(&c, full, &st, is_head,
                              have_range ? range : NULL, ims,
                              keepalive, &status, &sent);
        } else {
            /* A device, a fifo or a socket. Opening a fifo blocks until
             * somebody writes to it, and opening a device does whatever
             * that device does - neither belongs on the end of a URL. */
            status = 403;
            alive = send_error(&c, 403, NULL, keepalive, method);
        }

        log_request(addr, method, rel, status, sent);
        if (!alive || !keepalive)
            return;
    }
}

/* ── privileges ───────────────────────────────────────────────────
 *
 * Called AFTER bind, and the order is not a detail. Binding a port
 * below 1024 needs root, so `httpd -p 80` has to start as root; if the
 * privileges went first there would be no way to get the port at all.
 * Everything after the bind - parsing what strangers send, opening what
 * they name - runs as somebody who cannot write to /etc, cannot read
 * the SSH host key, and cannot reboot the board.
 *
 * The log file is opened before this too, for the same reason: root can
 * create /data/log/httpd.log, and the dropped-to user goes on writing
 * through the descriptor it already has. */
/* Returns the account it became, or NULL if it stayed as it was. */
static const char *drop_privileges(void)
{
    static lp_user_t u;

    if (lp_getuid() != 0)
        return NULL;

    static const char *candidates[] = { "www", "nobody", "user" };
    bool found = false;
    for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
        if (lp_user_by_name(candidates[i], &u) && u.uid != 0) {
            found = true;
            break;
        }

    if (!found) {
        dprintf(STDERR_FILENO,
                "httpd: no non-root account to drop to - staying as root.\n"
                "       add one with `useradd www` and restart.\n");
        return NULL;
    }

    lp_setgroups(0, NULL);
    lp_setgid(u.gid);
    if (lp_setuid(u.uid) < 0) {
        dprintf(STDERR_FILENO,
                "httpd: could not become %s - refusing to serve as root\n",
                u.name);
        lp_exit(1);
    }
    printf("httpd: running as %s (uid %d)\n", u.name, (int)u.uid);
    return u.name;
}

/* ── the accept loop ──────────────────────────────────────────────*/

static void usage(void)
{
    printf("usage: httpd [-p port] [-d dir] [-i addr] [-l file] [-f]\n\n");
    printf("  -p  port to listen on (default %d)\n", DEF_PORT);
    printf("  -d  directory to serve (default %s)\n", DEF_ROOT);
    printf("  -i  bind one address only, e.g. 127.0.0.1 (default: all)\n");
    printf("  -l  request log (default %s when in the background)\n", DEF_LOG);
    printf("  -f  stay in the foreground\n");
    printf("  -h  this text\n");
    printf("\nServes static files: GET and HEAD, index.html or a "
           "generated listing,\nranges so a big file can be resumed, and "
           "at most %d connections at once.\n", MAX_CONN);
    printf("Symlinks under the directory are never followed, and nothing "
           "outside it\nis reachable.\n");
    printf("\nIt is not started at boot and should not be. To have init "
           "keep it running:\n\n"
           "    service add httpd -f -d %s -l %s\n\n"
           "-f because init can only supervise a child that stays in the "
           "foreground,\nand -l because otherwise every request would be "
           "printed on the console.\n", DEF_ROOT, DEF_LOG);
}

int main(int argc, char **argv)
{
    int  port = DEF_PORT;
    bool foreground = false;
    const char *logpath = NULL;
    const char *bindaddr = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 2;
        } else if (strcmp(a, "-f") == 0) {
            foreground = true;
        } else if (strcmp(a, "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(a, "-d") == 0 && i + 1 < argc) {
            strlcpy(g_root, argv[++i], sizeof g_root);
        } else if (strcmp(a, "-l") == 0 && i + 1 < argc) {
            logpath = argv[++i];
        } else if (strcmp(a, "-i") == 0 && i + 1 < argc) {
            bindaddr = argv[++i];
        } else {
            dprintf(STDERR_FILENO, "httpd: %s?  try httpd -h\n", a);
            return 2;
        }
    }

    if (port < 1 || port > 65535) {
        dprintf(STDERR_FILENO,
                "httpd: %d is not a port. 1 to 65535, and under 1024 "
                "needs root.\n", port);
        return 2;
    }

    /* Trim a trailing slash so the root and the paths built on it join
     * cleanly, and so walk_path's prefix is exactly what it compares. */
    size_t rl = strlen(g_root);
    while (rl > 1 && g_root[rl - 1] == '/')
        g_root[--rl] = '\0';

    if (!lp_is_dir(g_root)) {
        dprintf(STDERR_FILENO,
                "httpd: %s is not a directory.\n"
                "       mkdir -p %s and put something in it, or name "
                "another with -d.\n", g_root, g_root);
        return 1;
    }

    u32 addr_be = 0;                    /* 0.0.0.0 - every interface */
    if (bindaddr && !ipv4_parse(bindaddr, &addr_be)) {
        dprintf(STDERR_FILENO,
                "httpd: %s is not an IPv4 address. try `ifconfig` to see "
                "what this machine has.\n", bindaddr);
        return 2;
    }

    long lfd = lp_socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        dprintf(STDERR_FILENO, "httpd: cannot make a socket (%ld)\n", -lfd);
        return 1;
    }

    /* Without SO_REUSEADDR, restarting the server fails for a minute or
     * two while the last connections sit in TIME_WAIT - which is
     * exactly when somebody is restarting it, having just changed
     * something. */
    int one = 1;
    lp_setsockopt((int)lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in_t sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u16)port);
    sa.sin_addr   = addr_be;

    long rc = lp_bind((int)lfd, &sa, sizeof sa);
    if (rc < 0) {
        if (-rc == 13 || -rc == 1)
            dprintf(STDERR_FILENO,
                    "httpd: not allowed to bind port %d - ports under "
                    "1024 need root.\n", port);
        else if (-rc == 98)
            dprintf(STDERR_FILENO,
                    "httpd: port %d is already in use. `netstat -l` says "
                    "by what.\n", port);
        else
            dprintf(STDERR_FILENO,
                    "httpd: cannot bind port %d (%ld)\n", port, -rc);
        lp_close((int)lfd);
        return 1;
    }

    if (lp_listen((int)lfd, MAX_CONN) < 0) {
        dprintf(STDERR_FILENO, "httpd: cannot listen on port %d\n", port);
        lp_close((int)lfd);
        return 1;
    }

    /* Opened as root, written to as somebody else - see the comment on
     * drop_privileges. */
    if (!logpath && !foreground)
        logpath = DEF_LOG;
    if (logpath) {
        long f = lp_open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f < 0) {
            dprintf(STDERR_FILENO,
                    "httpd: cannot write %s - is /data mounted?\n", logpath);
            lp_close((int)lfd);
            return 1;
        }
        g_logfd = (int)f;
    }

    /* Writing to a socket the browser has closed would otherwise kill
     * the child before it could log what it had been doing. */
    lp_signal_ignore(SIGPIPE);

    const char *as = drop_privileges();

    /* The directory was checked before the privileges went. Check it
     * again after, because a root-only path is a real mistake to make -
     * everything starts cleanly and then every single request is a 404
     * with nothing anywhere saying why. */
    /* lp_access, not lp_is_dir: stat() only needs the parent to be
     * traversable, so a directory nobody can enter still looks like a
     * perfectly good directory from outside it. R_OK|X_OK is the
     * question actually being asked - can this user list it and open
     * what is in it. */
    if (as && lp_access(g_root, R_OK | X_OK) < 0)
        dprintf(STDERR_FILENO,
                "httpd: the %s account cannot read %s, so every request "
                "will be refused.\n"
                "       chmod -R a+rX %s, or point -d at a directory that "
                "account can read.\n", as, g_root, g_root);

    char shown[16] = "0.0.0.0";
    if (addr_be)
        ipv4_format(addr_be, shown);
    printf("httpd: serving %s on %s:%d\n", g_root, shown, port);

    if (!foreground) {
        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "httpd: fork failed\n");
            return 1;
        }
        if (pid > 0)
            return 0;                   /* the parent leaves at once */
        lp_setsid();
    }

    int live = 0;
    for (;;) {
        /* Reap first, so `live` is right before the cap is tested. */
        int status;
        while (live > 0 && lp_waitpid(-1, &status, WNOHANG) > 0)
            live--;

        lp_pollfd_t pfd;
        pfd.fd = (int)lfd;
        pfd.events = LP_POLLIN;
        pfd.revents = 0;
        /* A one-second timeout, not an infinite wait: the loop has to
         * come round to reap children even when no new client arrives,
         * or a busy minute followed by a quiet hour leaves thirty
         * zombies and a server that thinks it is full. */
        long pr = lp_poll(&pfd, 1, 1000);
        if (pr <= 0)
            continue;

        sockaddr_in_t peer;
        u32 plen = sizeof peer;
        memset(&peer, 0, sizeof peer);
        long cfd = lp_accept((int)lfd, &peer, &plen, 0);
        if (cfd < 0)
            continue;

        char addr[16];
        ipv4_format(peer.sin_addr, addr);

        if (live >= MAX_CONN) {
            /* Refuse in the parent, without forking. A send timeout
             * first, because a client that has stopped reading must not
             * be able to block the accept loop on this one write. */
            conn_t tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.fd = (int)cfd;
            set_timeout((int)cfd, SO_SNDTIMEO_NEW, 2);
            send_error(&tmp, 503, "Retry-After: 2\r\n", false, "GET");
            log_request(addr, "-", "-", 503, 0);
            lp_close((int)cfd);
            continue;
        }

        pid_t pid = lp_fork();
        if (pid < 0) {
            conn_t tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.fd = (int)cfd;
            set_timeout((int)cfd, SO_SNDTIMEO_NEW, 2);
            send_error(&tmp, 503, "Retry-After: 5\r\n", false, "GET");
            log_request(addr, "-", "-", 503, 0);
            lp_close((int)cfd);
            continue;
        }
        if (pid == 0) {
            lp_close((int)lfd);         /* the child never accepts */
            serve((int)cfd, addr);
            lp_close((int)cfd);
            lp_exit(0);
        }
        lp_close((int)cfd);
        live++;
    }
}
