/* ec2 - talk to the EC2 instance metadata service, and do the two things
 * a machine has to do on AWS before anybody can reach it.
 *
 *   ec2 key             fetch this instance's SSH key and let it in
 *   ec2 meta <path>     print any metadata field
 *   ec2 id              instance id, type, zone, addresses
 *   ec2 userdata        print the user-data the instance was launched with
 *   ec2 hostname        set the hostname from the metadata
 *   ec2 -d              do all of the above once the network is up, and
 *                       keep watching in case it comes up late
 *
 * ── Why this exists ──
 *
 * On EC2 the SSH key is not in the image. It is chosen at launch and
 * handed to the instance through 169.254.169.254. An image that does not
 * fetch it boots perfectly and is unreachable forever - the one failure
 * where nothing is broken and nothing can be done.
 *
 * ── Why it is not a shell script ──
 *
 * IMDSv2 wants a PUT with a header to get a token, then a GET carrying
 * that token. Our wget takes a URL and nothing else: no method, no
 * headers. Extending wget for one caller would put AWS-shaped knobs on
 * a general tool. The metadata service is a fixed address with no DNS
 * and no TLS, so a socket and two hundred lines are the whole of it.
 *
 * v1 is still tried if the token PUT fails, because an instance whose
 * account leaves IMDSv1 enabled should not be locked out over it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

#define IMDS_IP   0xFEA9FEA9u          /* 169.254.169.254, as the bytes sit */
#define IMDS_PORT 80

/* One request. Returns the body length, or -1. Body is NUL-terminated. */
static long imds_req(const char *method, const char *path,
                     const char *hdr, char *out, size_t outsize)
{
    long fd = lp_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* The link-local address is answered by the hypervisor in under a
     * millisecond or not at all. Two seconds is already generous, and
     * anything longer just makes a machine with no metadata service -
     * a laptop, a VM on somebody's desk - look hung at boot. */
    s64 tv[2] = { 2, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);
    int syn = 2;
    lp_setsockopt((int)fd, IPPROTO_TCP, IPPROTO_TCP_SYNCNT, &syn, sizeof syn);

    sockaddr_in_t sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(IMDS_PORT);
    sa.sin_addr   = IMDS_IP;

    if (lp_connect((int)fd, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return -1;
    }

    char req[512];
    int rn = snprintf(req, sizeof req,
                      "%s %s HTTP/1.0\r\n"
                      "Host: 169.254.169.254\r\n"
                      "%s"
                      "Connection: close\r\n\r\n",
                      method, path, hdr ? hdr : "");
    if (rn <= 0 || lp_write((int)fd, req, (size_t)rn) < 0) {
        lp_close((int)fd);
        return -1;
    }

    /* Read it all, then split off the headers. The bodies here are a
     * key or an instance id; none of them is large. */
    char buf[8192];
    size_t n = 0;
    for (;;) {
        long r = lp_read((int)fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= sizeof buf - 1) break;
    }
    lp_close((int)fd);
    buf[n] = '\0';

    if (strncmp(buf, "HTTP/1.", 7) != 0)
        return -1;
    /* "HTTP/1.0 200 OK" - anything but 200 has no body worth having. */
    const char *sp = strchr(buf, ' ');
    if (!sp || sp[1] != '2')
        return -1;

    const char *body = strstr(buf, "\r\n\r\n");
    if (!body)
        return -1;
    body += 4;

    size_t blen = strlen(body);
    if (blen >= outsize) blen = outsize - 1;
    memcpy(out, body, blen);
    out[blen] = '\0';
    return (long)blen;
}

/* A token, or "" when the service only speaks v1. */
static void imds_token(char *tok, size_t size)
{
    tok[0] = '\0';
    char body[512];
    long n = imds_req("PUT", "/latest/api/token",
                      "X-aws-ec2-metadata-token-ttl-seconds: 21600\r\n",
                      body, sizeof body);
    if (n > 0 && n < (long)size)
        strlcpy(tok, body, size);
}

/* Fetch one metadata path, v2 first. */
static long imds_get(const char *path, char *out, size_t outsize)
{
    char tok[512];
    imds_token(tok, sizeof tok);

    if (tok[0]) {
        char hdr[600];
        snprintf(hdr, sizeof hdr, "X-aws-ec2-metadata-token: %s\r\n", tok);
        long n = imds_req("GET", path, hdr, out, outsize);
        if (n >= 0)
            return n;
    }
    return imds_req("GET", path, NULL, out, outsize);
}

static int on_ec2(void)
{
    char buf[64];
    return imds_get("/latest/meta-data/instance-id", buf, sizeof buf) > 0;
}

/* ── ec2 key ──────────────────────────────────────────────────────────
 *
 * The key goes where authkey already looks, so the merge on the next
 * boot keeps it and the three-places rule still holds. Appending only
 * when it is not already there means this is safe to run every boot and
 * on every network flap. */
static int install_key(void)
{
    char key[2048];
    long n = imds_get("/latest/meta-data/public-keys/0/openssh-key",
                      key, sizeof key);
    if (n <= 0) {
        dprintf(STDERR_FILENO,
                "ec2: no SSH key in the metadata.\n"
                "ec2:   The instance was launched without a key pair, or\n"
                "ec2:   169.254.169.254 is not answering yet.\n");
        return 1;
    }

    /* One line. The metadata sometimes ends without a newline. */
    char *nl = strchr(key, '\n');
    if (nl) *nl = '\0';
    if (!key[0])
        return 1;

    const char *path = "/root/.ssh/authorized_keys";
    lp_mkdir("/root/.ssh", 0700);

    char have[8192];
    have[0] = '\0';
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd >= 0) {
        long r = lp_read((int)fd, have, sizeof have - 1);
        if (r > 0) have[r] = '\0';
        lp_close((int)fd);
    }
    if (have[0] && strstr(have, key)) {
        printf("ec2: the key is already in %s\n", path);
        return 0;
    }

    fd = lp_open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ec2: cannot write %s\n", path);
        return 1;
    }
    lp_write((int)fd, key, strlen(key));
    lp_write((int)fd, "\n", 1);
    lp_close((int)fd);
    lp_chmod(path, 0600);

    printf("ec2: added the instance key to %s\n", path);
    return 0;
}

static int set_hostname(void)
{
    char h[256];
    if (imds_get("/latest/meta-data/local-hostname", h, sizeof h) <= 0)
        return 1;
    char *nl = strchr(h, '\n');
    if (nl) *nl = '\0';
    if (!h[0])
        return 1;
    /* /proc/sys/kernel/hostname, the same door `hostname` uses. There is
     * no sethostname in this libc and one caller does not earn one. */
    long hf = lp_open("/proc/sys/kernel/hostname", O_WRONLY, 0);
    if (hf < 0) {
        dprintf(STDERR_FILENO, "ec2: cannot set the hostname\n");
        return 1;
    }
    lp_write((int)hf, h, strlen(h));
    lp_close((int)hf);
    printf("ec2: hostname %s\n", h);
    return 0;
}

static void show_one(const char *label, const char *path)
{
    char v[512];
    if (imds_get(path, v, sizeof v) > 0) {
        char *nl = strchr(v, '\n');
        if (nl) *nl = '\0';
        printf("  %-18s %s\n", label, v);
    }
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "id";

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        printf("usage:\n");
        printf("  ec2 key            let this instance's SSH key in\n");
        printf("  ec2 id             instance id, type, zone, addresses\n");
        printf("  ec2 meta <path>    any metadata field\n");
        printf("  ec2 userdata       the user-data it was launched with\n");
        printf("  ec2 hostname       set the hostname from the metadata\n");
        printf("  ec2 -d             key + hostname, waiting for the network\n");
        return 0;
    }

    if (strcmp(cmd, "key") == 0)
        return install_key();

    if (strcmp(cmd, "hostname") == 0)
        return set_hostname();

    if (strcmp(cmd, "meta") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO, "ec2: which metadata path?\n");
            return 2;
        }
        char path[512], v[4096];
        if (argv[2][0] == '/')
            strlcpy(path, argv[2], sizeof path);
        else
            snprintf(path, sizeof path, "/latest/meta-data/%s", argv[2]);
        long n = imds_get(path, v, sizeof v);
        if (n < 0) {
            dprintf(STDERR_FILENO, "ec2: %s: no answer\n", path);
            return 1;
        }
        printf("%s\n", v);
        return 0;
    }

    if (strcmp(cmd, "userdata") == 0) {
        char v[8192];
        long n = imds_get("/latest/user-data", v, sizeof v);
        if (n <= 0) {
            printf("ec2: no user-data\n");
            return 1;
        }
        printf("%s\n", v);
        return 0;
    }

    if (strcmp(cmd, "-d") == 0) {
        /* The address arrives after rc has finished - dhcp is a
         * supervised service, not a step in rc. So this waits rather
         * than deciding at boot that there is no metadata service.
         *
         * After the work is done it keeps sleeping instead of exiting,
         * because the supervisor restarts anything that exits and a
         * one-shot would become a restart loop. */
        for (int tries = 0; tries < 120; tries++) {
            if (on_ec2()) {
                install_key();
                set_hostname();
                break;
            }
            lp_sleep_ms(2000);
        }
        for (;;)
            lp_sleep_ms(3600000);
    }

    if (strcmp(cmd, "id") == 0) {
        if (!on_ec2()) {
            printf("ec2: 169.254.169.254 is not answering."
                   " This is not an EC2 instance,\n"
                   "ec2:   or the network is not up yet.\n");
            return 1;
        }
        show_one("instance-id",  "/latest/meta-data/instance-id");
        show_one("type",         "/latest/meta-data/instance-type");
        show_one("zone",         "/latest/meta-data/placement/availability-zone");
        show_one("private-ip",   "/latest/meta-data/local-ipv4");
        show_one("public-ip",    "/latest/meta-data/public-ipv4");
        show_one("hostname",     "/latest/meta-data/local-hostname");
        show_one("ami",          "/latest/meta-data/ami-id");
        return 0;
    }

    dprintf(STDERR_FILENO, "ec2: %s? try `ec2 -h`\n", cmd);
    return 2;
}
