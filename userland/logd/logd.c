/* logd - collect log messages into a file that survives a reboot.
 *
 *   logd              run in the foreground (init supervises it)
 *   logd -f <path>    where to write (default /data/log/messages)
 *   logd -s <KB>      rotate when the file passes this size (default 512)
 *
 * Without this, everything a program prints goes to the console and is
 * gone the moment it scrolls. If the board wedges at 3am there is no
 * record of what happened - which is exactly when you want one.
 *
 * Two sources are collected:
 *   /dev/kmsg     the kernel's own ring buffer, read as a stream
 *   /dev/log      a unix datagram socket, where our programs can send
 *                 a line without caring where it ends up
 *
 * The file is rotated, not grown forever: at the limit it becomes
 * messages.1 and a fresh one starts. Exactly one old file is kept.
 * An SD card is small and this must never be what fills it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define DEFAULT_PATH  "/data/log/messages"
#define DEFAULT_KB    512
#define SOCK_PATH     "/dev/log"
#define KMSG_PATH     "/dev/kmsg"

#define AF_UNIX       1
#define SOCK_DGRAM_   2
#define SOCK_NONBLOCK_ 04000   /* O_NONBLOCK, as socket() takes it */

/* struct sockaddr_un { u16 family; char path[108]; } */
typedef struct {
    u16  family;
    char path[108];
} sockaddr_un_t;

static const char *log_path = DEFAULT_PATH;
static long        max_bytes = DEFAULT_KB * 1024;
static long        written   = 0;
static int         out_fd    = -1;

static void open_log(void)
{
    long fd = lp_open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "logd: cannot open %s (%ld)\n", log_path, -fd);
        out_fd = -1;
        return;
    }
    out_fd = (int)fd;

    lp_stat_t st;
    written = (lp_stat(log_path, &st, true) == 0) ? (long)st.size : 0;
}

/* At the limit, move the file aside and start a new one.
 *
 * Two generations, not one. When something goes wrong the interesting
 * lines are usually just before the current file starts - and guard
 * deletes the oldest log when the disk is nearly full, which with a
 * single generation means the evidence goes exactly when a full disk is
 * the thing being investigated. Two files at 512KB is 1MB, which is
 * nothing against a partition measured in gigabytes.
 *
 *   messages     what is happening now
 *   messages.1   the one before
 *   messages.2   the one before that, deleted here to make room
 */
static void rotate(void)
{
    char first[512], second[512];

    if (snprintf(first,  sizeof(first),  "%s.1", log_path) >= (int)sizeof(first))
        return;
    if (snprintf(second, sizeof(second), "%s.2", log_path) >= (int)sizeof(second))
        return;

    if (out_fd >= 0)
        lp_close(out_fd);

    lp_unlink(second);
    lp_rename(first, second);
    lp_rename(log_path, first);
    open_log();
}

/* ── The authentication log ───────────────────────────────────────────
 *
 * dropbear writes its login attempts to the console, and everything on
 * the console lands in messages along with the kernel's chatter about
 * USB devices and filesystems. Which means that after a break-in, the
 * one question worth asking - who logged in, from where, and when - is
 * answered by reading a megabyte of unrelated text, if the lines have
 * not already been rotated out by ordinary noise.
 *
 * So they get their own file, with its own rotation. It is small, it
 * fills slowly, and an attacker cannot push the record of their own
 * arrival off the end of it by generating traffic.
 *
 * Matching by text rather than by process: dropbear's messages are
 * distinctive and there is no syslog facility to key on here. */
static int  auth_fd    = -1;
static long auth_bytes = 0;
static char auth_path[512];

static bool is_auth_line(const char *msg, size_t len)
{
    /* Bounded copy, because the message is not NUL terminated. */
    char line[256];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, msg, n);
    line[n] = '\0';

    static const char *marks[] = {
        "Password auth succeeded",
        "Pubkey auth succeeded",
        "Public key auth succeeded",
        "Auth succeeded",
        "Bad password",
        "Login attempt",
        "Exit before auth",
        "Exit (",
        "exit before auth",
        "Child connection from",
        "bad packet length",
        NULL
    };

    for (int i = 0; marks[i]; i++)
        if (strstr(line, marks[i]))
            return true;
    return false;
}

static void auth_open(void)
{
    if (auth_path[0] == '\0')
        return;
    long fd = lp_open(auth_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    auth_fd = (int)fd;

    lp_stat_t st;
    auth_bytes = (lp_stat(auth_path, &st, true) == 0) ? (long)st.size : 0;
}

static void auth_emit(const char *head, int hlen, const char *msg, size_t len)
{
    if (auth_fd < 0) {
        auth_open();
        if (auth_fd < 0)
            return;
    }

    lp_write(auth_fd, head, (size_t)hlen);
    lp_write(auth_fd, msg, len);
    if (len == 0 || msg[len - 1] != '\n')
        lp_write(auth_fd, "\n", 1);

    auth_bytes += hlen + (long)len + 1;

    /* Its own limit, a quarter of the main one: this file grows only
     * when someone connects, so it takes a long time to fill, and a
     * long history is exactly what it is for. */
    if (auth_bytes >= max_bytes / 4) {
        char old[520];
        if (snprintf(old, sizeof(old), "%s.1", auth_path) < (int)sizeof(old)) {
            lp_close(auth_fd);
            auth_fd = -1;
            lp_unlink(old);
            lp_rename(auth_path, old);
            auth_open();
        }
    }
}

static void emit(const char *tag, const char *msg, size_t len)
{
    if (out_fd < 0) {
        open_log();
        if (out_fd < 0)
            return;
    }

    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);

    char head[96];
    int  hlen = snprintf(head, sizeof(head),
                         "%d-%02d-%02d %02d:%02d:%02d %s: ",
                         tm.year, tm.mon, tm.day,
                         tm.hour, tm.min, tm.sec, tag);

    lp_write(out_fd, head, (size_t)hlen);
    lp_write(out_fd, msg, len);
    if (len == 0 || msg[len - 1] != '\n')
        lp_write(out_fd, "\n", 1);

    written += hlen + (long)len + 1;
    if (written >= max_bytes)
        rotate();

    /* Anything about somebody arriving goes to both files: the main log
     * keeps the sequence of events in context, the auth log keeps it
     * where it can be found. */
    if (is_auth_line(msg, len))
        auth_emit(head, hlen, msg, len);
}

/* /dev/kmsg lines look like "6,123,456789,-;the message".
 * Everything before the ';' is metadata we do not need. */
static const char *strip_kmsg(const char *line, size_t *len)
{
    for (size_t i = 0; i < *len; i++) {
        if (line[i] == ';') {
            *len -= i + 1;
            return line + i + 1;
        }
    }
    return line;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            long kb = atoi(argv[++i]);
            if (kb < 16) kb = 16;
            max_bytes = kb * 1024;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: logd [-f path] [-s KB]\n");
            printf("  -f <path>  where to write (default %s)\n", DEFAULT_PATH);
            printf("  -s <KB>    rotate past this size (default %d)\n", DEFAULT_KB);
            printf("\nRead the log with:  cat %s\n", DEFAULT_PATH);
            printf("Logins are also kept apart, in auth beside it, with\n");
            printf("their own rotation - so a break-in cannot be pushed\n");
            printf("off the end of the log by ordinary traffic.\n");
            return 0;
        } else {
            dprintf(STDERR_FILENO, "logd: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    /* The auth log sits beside the main one, whatever it was named. */
    {
        char base[512];
        strlcpy(base, log_path, sizeof(base));
        char *slash = strrchr(base, '/');
        if (slash) {
            *slash = '\0';
            snprintf(auth_path, sizeof(auth_path), "%s/auth", base);
        } else {
            strlcpy(auth_path, "auth", sizeof(auth_path));
        }
    }

    open_log();
    if (out_fd < 0)
        return 1;

    emit("logd", "started", 7);

    /* The socket other programs send to. Datagrams, so a sender never
     * blocks on us and never needs to connect. */
    lp_unlink(SOCK_PATH);
    /* SOCK_NONBLOCK, and it is not a refinement.
     *
     * Without it this socket blocks, and the poll loop below reads
     * kernel records first and the socket second - so logd read the
     * first 32 records of the boot, reached the socket, and waited
     * there for a datagram that never came, because nothing in this
     * system had ever been taught to send one. It stayed that way for
     * the rest of the uptime: alive, holding an open log file, and
     * collecting nothing. Measured on a running board: 33 lines in
     * /data/log/messages, the last one from three seconds into the
     * boot, while the kernel ring buffer went on filling.
     *
     * So the log was not merely missing our own programs' messages -
     * it was missing everything after the first moment of the boot,
     * including every kernel message about the disk. */
    long sfd = lp_socket(AF_UNIX, SOCK_DGRAM_ | SOCK_NONBLOCK_, 0);
    if (sfd >= 0) {
        sockaddr_un_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.family = AF_UNIX;
        strlcpy(addr.path, SOCK_PATH, sizeof(addr.path));
        if (lp_bind((int)sfd, &addr, sizeof(addr)) < 0) {
            lp_close((int)sfd);
            sfd = -1;
        }
    }

    long kfd = lp_open(KMSG_PATH, O_RDONLY | O_NONBLOCK, 0);

    /* Poll both sources. Neither is busy, so a short sleep between
     * passes costs nothing and keeps this off the CPU. */
    char buf[2048];
    for (;;) {
        bool did = false;

        if (kfd >= 0) {
            for (int n = 0; n < 32; n++) {
                long r = lp_read((int)kfd, buf, sizeof(buf) - 1);
                if (r <= 0)
                    break;
                size_t len = (size_t)r;
                const char *msg = strip_kmsg(buf, &len);
                emit("kernel", msg, len);
                did = true;
            }
        }

        if (sfd >= 0) {
            for (int n = 0; n < 32; n++) {
                long r = lp_recvfrom((int)sfd, buf, sizeof(buf) - 1, 0, NULL, NULL);
                if (r <= 0)
                    break;
                emit("system", buf, (size_t)r);
                did = true;
            }
        }

        lp_sleep_ms(did ? 50 : 500);
    }
}
