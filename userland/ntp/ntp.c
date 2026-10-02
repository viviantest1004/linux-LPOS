/* ntp - set the system clock from the network.
 *
 *   ntp                  ask the default servers and set the clock
 *   ntp <server|IP>...   ask the servers you name
 *   ntp -r               no network: restore the last saved time
 *   ntp -d               daemon: save the time regularly and resync
 *
 * Why this is needed:
 *   The Pi Zero 2 W has no battery-backed clock. At power-on the kernel
 *   clock starts at 1 January 1970. Try HTTPS in that state and every
 *   server certificate looks like it starts in the future, so validation
 *   fails across the board. TLS needs the clock to be right.
 *
 * How:
 *   SNTP (RFC 4330). Send a 48-byte NTP packet to UDP 123 and read the
 *   transmit timestamp out of the reply. No precise synchronisation - no
 *   drift correction, no comparing servers. Seconds are enough for certs.
 *
 *   Host names are resolved here, by asking the nameserver in
 *   /etc/resolv.conf for an A record. Our libc has no resolver.
 *
 * The time we set is saved to /data/.clock, so the next boot can start
 * from it with -r even with no network. Far better than 1970.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* NTP counts from 1900-01-01, unix time from 1970-01-01.
 * This is the seconds between them: 70 years including 17 leap days. */
#define NTP_UNIX_DELTA  2208988800LL

#define NTP_PORT        123
#define DNS_PORT        53
#define CLOCK_FILE      "/data/.clock"
/* Present: automatic time is off (lp-time ntp off). */
#define NTP_OFF_FILE    "/etc/lp/ntp-off"

/* 2020-01-01. An answer earlier than this means something is wrong. */
#define SANITY_MIN      1577836800LL
/* 2100-01-01 */
#define SANITY_MAX      4102444800LL

static const char *DEFAULT_SERVERS[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
    "time.google.com",
    NULL
};

/* For the daemon: stay silent on failure. Spraying errors onto the
 * console every hour would make the serial console unusable. */
static bool quiet_mode = false;

/* ── Receive timeout ──────────────────────────────────────────────
 * Boot must not stall on a server that never answers. */
static void set_timeout(int fd, long seconds)
{
    /* struct __kernel_sock_timeval { s64 tv_sec; s64 tv_usec; } */
    s64 tv[2] = { seconds, 0 };
    lp_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));
}

/* Resolving a name is in the libc now - ntp was not the only thing that
 * needed it, and two copies of a DNS client is one too many. */

/* ── SNTP ───────────────────────────────────────────────────────
 * Unix seconds on success, 0 on failure. */
static s64 query_ntp(const char *server)
{
    u32 addr = net_resolve(server);
    if (addr == 0) {
        /* A name that will not resolve and a server that will not answer
         * have nothing in common. Say which, or nobody knows where to look. */
        if (!quiet_mode)
            dprintf(STDERR_FILENO, "ntp: %s: cannot resolve that name\n", server);
        return 0;
    }

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    set_timeout((int)fd, 3);

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port   = htons(NTP_PORT);
    to.sin_addr   = addr;

    /* 48 bytes, and only the first one has to be filled in:
     *   LI=0 (no warning) VN=4 (NTPv4) Mode=3 (client)
     *   0<<6 | 4<<3 | 3 = 0x23 */
    u8 pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;

    /* A random transmit timestamp, which the server copies back into
     * the originate field. It is the only thing that ties a reply to
     * this request: the packet used to go out with that field all
     * zeros, and the reply was accepted on the strength of being 48
     * bytes long from anybody at all. Setting the clock is not a small
     * thing to hand out - the reason this program exists is TLS
     * certificate validity, and the value is written to /data/.clock so
     * it survives the reboot. */
    u8 nonce[8];
    if (lp_getrandom(nonce, sizeof nonce, 0) != (long)sizeof nonce)
        for (int i = 0; i < 8; i++)
            nonce[i] = (u8)((lp_monotonic_ms() >> (i * 8)) ^ (i * 37 + 11));
    memcpy(pkt + 40, nonce, 8);

    /* And connect, so the kernel drops datagrams from anyone but the
     * server we asked. */
    if (lp_connect((int)fd, &to, sizeof(to)) < 0) {
        lp_close((int)fd);
        return 0;
    }

    s64 result = 0;
    if (lp_sendto((int)fd, pkt, sizeof(pkt), 0, &to, sizeof(to)) > 0) {
        u8   resp[48];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);
        if (n < 0 && !quiet_mode) {
            char ip[16];
            ipv4_format(addr, ip);
            dprintf(STDERR_FILENO, "ntp: %s (%s): no reply\n", server, ip);
        }
        if (n == (long)sizeof(resp)) {
            int mode = resp[0] & 0x07;
            int vn   = (resp[0] >> 3) & 0x07;

            /* Mode 4 is "server", the version has to be one we spoke,
             * stratum 0 is a kiss-of-death packet and carries no time,
             * and the originate field must be the nonce we sent. */
            bool sane = (mode == 4) && (vn >= 1 && vn <= 4) &&
                        (resp[1] != 0) &&
                        memcmp(resp + 24, nonce, 8) == 0;

            if (sane) {
                /* transmit timestamp at offset 40; high 4 bytes = seconds */
                u32 secs = ((u32)resp[40] << 24) | ((u32)resp[41] << 16) |
                           ((u32)resp[42] << 8)  |  (u32)resp[43];
                if (secs != 0)
                    result = (s64)secs - NTP_UNIX_DELTA;
            } else if (!quiet_mode) {
                dprintf(STDERR_FILENO,
                        "ntp: %s: reply did not match the request -"
                        " ignoring it\n", server);
            }
        }
    }

    lp_close((int)fd);
    return result;
}

static s64 query_ntp_quiet(const char *server)
{
    bool saved = quiet_mode;
    quiet_mode = true;
    s64 t = query_ntp(server);
    quiet_mode = saved;
    return t;
}

/* ── Save and restore ────────────────────────────────────────────
 *
 * A PC and an EC2 instance have a battery-backed clock; the kernel
 * reads it at boot and it counts on while the power is off. So the
 * first thing a fetched time does is go back into it.
 *
 * A Pi Zero 2 W has no such clock, and this file stands in for one.
 * It does not advance while the machine is off, so a board switched on
 * a week later is a week behind - but a week behind is a working HTTPS
 * handshake and 1970 is not. */

static void save_clock(s64 t)
{
    lp_rtc_write(t);

    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);
    long fd = lp_open(CLOCK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;                      /* no /data: nothing to do */
    lp_write((int)fd, buf, (size_t)len);
    lp_close((int)fd);
}

static s64 load_clock(void)
{
    char buf[32];
    long fd = lp_open(CLOCK_FILE, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    /* Our libc has no atoll, and unix seconds are already 10 digits -
     * too many for a 32-bit atoi - so parse it here. */
    s64 v = 0;
    for (const char *c = buf; *c >= '0' && *c <= '9'; c++)
        v = v * 10 + (*c - '0');
    return v;
}

static void report(s64 t)
{
    lp_tm_t tm;
    lp_gmtime(t, &tm);
    printf("ntp: %d-%02d-%02d %02d:%02d:%02d UTC\n",
           tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
}

/* ── The daemon ──────────────────────────────────────────────────
 * This board has no battery-backed clock. Pull the power and time stops
 * dead - that is hardware, and no software can fix it.
 *
 * What we can do is write the time down often. Then the next boot picks
 * up from just before the power went. How often we save is the worst
 * case error: at every 30s, a sudden power cut costs under 30 seconds.
 *
 * The card's endurance is not a worry: 32 bytes every 30s is 92KB a day,
 * rewriting the same block, and wear levelling spreads that around.
 *
 * We also resync from the network hourly. Save-and-restore alone falls
 * behind by however long the machine was off. */
#define SAVE_EVERY_SEC   30
#define RETRY_AFTER_SEC   60   /* try again a minute after a failure */
#define RESYNC_EVERY_SEC 3600

static int run_daemon(const char **servers)
{
    printf("ntp: daemon started (saving every %ds, resyncing every %dmin)\n",
           SAVE_EVERY_SEC, RESYNC_EVERY_SEC / 60);

    s64 last_sync = 0;

    for (;;) {
        s64 now = lp_time();

        /* Only save a plausible time. Saving 1970 would send the next boot
         * back to it, which is worse than nothing. */
        if (now >= SANITY_MIN)
            save_clock(now);

        /* Automatic time turned off (lp-time ntp off, or Settings >
         * Date & Time): keep saving the clock, but never set it from the
         * network - a time set by hand must stay what it was set to. */
        if (lp_access(NTP_OFF_FILE, F_OK) == 0) {
            last_sync = 0;          /* turned back on: sync at once */
            lp_sleep_ms(SAVE_EVERY_SEC * 1000);
            continue;
        }

        if (now - last_sync >= RESYNC_EVERY_SEC || last_sync == 0) {
            bool got_it = false;
            for (int i = 0; servers[i]; i++) {
                s64 t = query_ntp_quiet(servers[i]);
                if (t >= SANITY_MIN && t <= SANITY_MAX) {
                    if (lp_settime(t) == 0)
                        save_clock(t);
                    got_it = true;
                    break;
                }
            }

            /* A failed attempt must not count as a sync.
             *
             * last_sync was set either way, so the first try - which on
             * a first boot happens seconds after init starts, before
             * WiFi has associated - failed and then booked itself an
             * hour of silence. The board ran that whole hour believing
             * it was 1970: every HTTPS certificate outside its validity
             * window, pkg and python failing on dates, every log line
             * stamped wrong, and nothing said why.
             *
             * On failure, come back in a minute instead. */
            if (got_it)
                last_sync = lp_time();
            else
                last_sync = lp_time() - RESYNC_EVERY_SEC + RETRY_AFTER_SEC;
        }

        lp_sleep_ms(SAVE_EVERY_SEC * 1000);
    }
    return 0;   /* not reached */
}

/* The newest timestamp on anything in a directory, and on the directory
 * itself. Zero if it cannot be read.
 *
 * A file's mtime is a fact about the past: whatever wrote it did so at
 * that moment, so the current time cannot be earlier. That makes the
 * filesystem a clock of last resort, and a much better one than the
 * build date:
 *
 *   /boot   was written by a PC when the card was made. Its mtimes are
 *           that PC's clock - usually minutes before the first boot.
 *   /data   was written by this board the last time it ran. Its mtimes
 *           move forward on their own, every boot, for ever.
 *
 * Neither is the right time and neither claims to be. Both are true
 * lower bounds, and the true bound closest to now is the one to use. */
static s64 newest_mtime(const char *dir)
{
    s64 newest = 0;
    lp_stat_t st;

    if (lp_stat(dir, &st, true) == 0)
        newest = st.mtime;

    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return newest;

    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        /* getdents64 packs variable-length records back to back:
         *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char name[] */
        for (long off = 0; off < n; ) {
            u16 reclen;
            memcpy(&reclen, buf + off + 16, sizeof reclen);
            if (reclen == 0)
                break;
            const char *name = buf + off + 19;
            off += reclen;
            if (name[0] == '.')
                continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, name);
            if (lp_stat(path, &st, false) == 0 && st.mtime > newest)
                newest = st.mtime;
        }
    }
    lp_close((int)fd);
    return newest;
}

/* A file holding one decimal number of seconds since 1970, and nothing
 * else. Missing or unreadable is 0, which loses to everything. */
static s64 load_epoch_file(const char *path)
{
    char buf[32];
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return (s64)strtoll(buf, NULL, 10);
}

int main(int argc, char **argv)
{
    /* -r: restore the saved time without a network. Used early in boot. */
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        s64 stored = load_clock();
        s64 saved = stored;
        s64 built = load_epoch_file("/etc/build-epoch");

        /* A battery-backed clock that reads a time after this image was
         * built is the time, and nothing below may overrule it. It kept
         * counting while the power was off, it is where a clock set by
         * hand is kept (lp-time set ends with hwclock -w), and the kernel
         * has already applied it (CONFIG_RTC_HCTOSYS).
         *
         * The saved time and the file dates further down are lower bounds
         * for a machine without one, and they are only true lower bounds
         * while the clock has never been wrong. They used to be taken over
         * the hardware clock whenever they were later: a clock set back an
         * hour by hand left files dated in the hour it skipped, and the
         * next start moved the clock forward to them - "starting from the
         * newest thing on the card" - so a time set in Settings came back
         * wrong after every restart, on a PC with a perfectly good clock.
         *
         * A reading from before the build is a clock that lost its battery
         * (firmware puts it back to 2000, or to its own release date):
         * that one is worth less than the lower bounds, and falls through
         * to them. */
        s64 rtc = 0;
        if (lp_rtc_read(&rtc) && rtc >= SANITY_MIN && rtc >= built) {
            if (lp_time() < rtc && lp_settime(rtc) < 0) {
                dprintf(STDERR_FILENO, "ntp: cannot set the clock (are you root?)\n");
                return 1;
            }
            printf("ntp: the hardware clock has the time\n");
            report(rtc);
            return 0;
        }

        /* When the image was built, as a floor.
         *
         * A first boot has no saved time, and a board with no
         * battery-backed clock therefore starts at 1970 - where it stays
         * until the network comes up. If the network is the thing that
         * is broken, that is for ever, and every line in the log carries
         * the same 1970-01-01 timestamp. You cannot tell what happened
         * before what, which is exactly what you need when the network
         * is what you are trying to fix.
         *
         * This image cannot have been running before it was built, so
         * the build time is a lower bound that is always true and is
         * never in the future. It is not the right time and does not
         * claim to be - report() says where the time came from - but it
         * orders the log and it stops certificate checks failing by
         * fifty years instead of by minutes. */
        /* Every lower bound we have, best last. Each one is a moment
         * this machine cannot possibly be earlier than. */
        s64 floor = built;                                   /* image built */
        s64 t;
        if ((t = newest_mtime("/boot")) > floor) floor = t;  /* card written */
        if ((t = newest_mtime("/data")) > floor) floor = t;  /* last run */
        if ((t = newest_mtime("/data/log")) > floor) floor = t;
        if (floor > saved)
            saved = floor;
        /* A hardware clock that failed the test above can still be
         * ahead of all of that. */
        if (rtc > saved)
            saved = rtc;

        if (saved < SANITY_MIN) {
            dprintf(STDERR_FILENO, "ntp: no saved time\n");
            return 1;
        }
        bool from_floor = (floor == saved && stored < floor);
        /* Never move the clock backwards. */
        if (lp_time() >= saved) {
            /* Say what it was anyway. "Already ahead" on its own leaves
             * you unable to tell a good saved time from a floor of
             * 1970, and that is exactly the question being asked when
             * somebody runs this by hand. */
            printf("ntp: the clock is already ahead of that\n");
            report(saved);
            return 0;
        }
        if (lp_settime(saved) < 0) {
            dprintf(STDERR_FILENO, "ntp: cannot set the clock (are you root?)\n");
            return 1;
        }
        if (from_floor && stored == 0)
            printf("ntp: no time was saved - starting from the newest thing"
                   " on the card, which cannot be in the future\n");
        else if (from_floor)
            printf("ntp: the newest thing on the card is later than the saved"
                   " time - starting from it\n");
        else
            printf("ntp: restored the saved time (not from the network)\n");
        report(saved);
        return 0;
    }

    bool daemon = false;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon = true;
        argv++;
        argc--;
    }

    const char **servers = DEFAULT_SERVERS;
    const char  *from_args[8];
    if (argc > 1) {
        int n = 0;
        for (int i = 1; i < argc && n < 7; i++)
            from_args[n++] = argv[i];
        from_args[n] = NULL;
        servers = from_args;
    }

    if (daemon)
        return run_daemon(servers);

    for (int i = 0; servers[i]; i++) {
        s64 t = query_ntp(servers[i]);
        if (t < SANITY_MIN || t > SANITY_MAX) {
            if (t != 0)
                dprintf(STDERR_FILENO, "ntp: %s gave an implausible answer\n", servers[i]);
            continue;
        }
        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO, "ntp: cannot set the clock (are you root?)\n");
            return 1;
        }
        printf("ntp: got the time from %s\n", servers[i]);
        report(t);
        save_clock(t);
        return 0;
    }

    dprintf(STDERR_FILENO,
            "ntp: could not get the time.\n"
            "     UDP 123 may be blocked (common on public and office networks),\n"
            "     or there may be no address yet. To use another server:\n"
            "       ntp <server>\n");
    return 1;
}
