/* authkey - make sure there is still a way in.
 *
 *   authkey            merge the keys and report
 *   authkey new        make a key here, and print how to connect with it
 *   authkey add        read a public key from a file, or from a paste
 *   authkey -l         list what is authorized
 *   authkey -q         quiet; the exit code is the answer
 *
 * Exits non-zero when no key is authorized, which means nobody can log
 * in - including whoever owns the machine.
 *
 * ── The problem this exists for ──
 * SSH is how this board is used. The key that allows it lives in
 * /root/.ssh/authorized_keys, and /root is a bind mount from /data - so
 * anything that destroys the data partition, ransomware or a mistake or
 * a dying card, takes the key with it. The machine then boots perfectly
 * and is unreachable forever.
 *
 * Root on the machine can reach anything the machine can reach, so this
 * is not about preventing that. It is about making sure that one reboot
 * always gets you back in. Three places hold a copy, and they are
 * progressively harder to destroy:
 *
 *   /data/root/.ssh/authorized_keys   the live one. Assume it is gone.
 *   /boot/authorized_keys             FAT, mounted read-only, editable
 *                                     from any PC with a card reader.
 *                                     Root can still remount and wipe
 *                                     it, but nothing else can.
 *   /etc/authorized_keys              inside the kernel image. Unpacked
 *                                     into RAM at every boot, read-only,
 *                                     and not reachable from any
 *                                     filesystem. Short of writing a new
 *                                     card, this one cannot be removed.
 *
 * Every boot, the outer two are merged into the live one. A key already
 * there is left alone, so keys added by hand on the machine survive, and
 * nothing is duplicated.
 *
 * ── Getting the first key in ──
 * The property above has a cost that went unnoticed for a long time: a
 * freshly burned card lets NOBODY in over the network. Password
 * authentication is not compiled into dropbear at all, on purpose, so
 * the only route was to shut the board down, take the card out, find a
 * computer with a slot, and write authorized_keys by hand. For a board
 * three metres up a wall that is not a route, and "SSH is too hard to
 * get into" is how a machine ends up with its firewall off instead.
 *
 * `authkey new` closes that. It makes a keypair here, authorises the
 * public half, leaves the private half on the FAT boot partition where
 * any computer can read it, and prints the ssh command with this
 * machine's own address already in it. Nothing is weakened: still no
 * passwords, the key is generated from the kernel's randomness and is
 * different on every machine, and the private half never existed
 * anywhere else.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

#define LIVE   "/root/.ssh/authorized_keys"
#define BOOT   "/boot/authorized_keys"
#define IMAGE  "/etc/authorized_keys"

#define MAX_KEYS 32
#define KEY_MAX  1024

static char keys[MAX_KEYS][KEY_MAX];
static int  nkeys;

/* A line is a key if it is not blank and not a comment. No parsing of
 * the key itself: dropbear decides what is valid, and a line we did not
 * understand is one we should not be throwing away. */
/* Does this line look like a public key?
 *
 * It used to be "not blank and not a comment", which was enough while
 * the only callers were files this system had written itself. `authkey
 * add` reads what a person pasted, and the commonest way that goes
 * wrong is a key broken across two lines by the terminal - so half a key
 * would have been accepted as a key, sat in authorized_keys looking
 * right, and simply never let anyone in.
 *
 * So the type has to be one SSH actually uses, and there has to be a
 * base64 body after it. The comment at the end is optional; everything
 * else is not. */
static bool is_key_line(const char *line)
{
    static const char *TYPES[] = {
        "ssh-ed25519", "ssh-rsa", "ssh-dss",
        "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
        "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com",
        NULL
    };

    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#')
        return false;

    /* An options field can come first ("no-pty,command=..." then the
     * type). Skip one if it is there - it never contains a space. */
    const char *p = line;
    for (int round = 0; round < 2; round++) {
        for (int i = 0; TYPES[i]; i++) {
            size_t n = strlen(TYPES[i]);
            if (strncmp(p, TYPES[i], n) == 0 && (p[n] == ' ' || p[n] == '\t')) {
                const char *body = p + n;
                while (*body == ' ' || *body == '\t') body++;
                /* A real body is at least 40 characters of base64. A
                 * truncated paste is usually much shorter than that and
                 * always stops mid-alphabet. */
                int len = 0;
                while (body[len] && body[len] != ' ' && body[len] != '\t') {
                    char c = body[len];
                    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                           || (c >= '0' && c <= '9') || c == '+' || c == '/'
                           || c == '=';
                    if (!ok)
                        return false;
                    len++;
                }
                return len >= 40;
            }
        }
        /* Not a type: it may be an options field. Step past one word and
         * try again, once. */
        const char *sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) return false;
        while (*sp == ' ' || *sp == '\t') sp++;
        p = sp;
    }
    return false;
}

/* The part that identifies a key, ignoring the trailing comment people
 * put their email address in - so the same key added twice under two
 * different names is still the same key. */
static void key_body(const char *line, char *out, size_t size)
{
    while (*line == ' ' || *line == '\t')
        line++;

    /* type SPACE base64 SPACE comment */
    const char *sp = strchr(line, ' ');
    if (!sp) {
        strlcpy(out, line, size);
        return;
    }
    const char *body = sp + 1;
    while (*body == ' ') body++;

    const char *end = strchr(body, ' ');
    size_t len = end ? (size_t)(end - body) : strlen(body);
    if (len >= size) len = size - 1;
    memcpy(out, body, len);
    out[len] = '\0';
}

static bool already_have(const char *line)
{
    char body[KEY_MAX], other[KEY_MAX];
    key_body(line, body, sizeof(body));

    for (int i = 0; i < nkeys; i++) {
        key_body(keys[i], other, sizeof(other));
        if (strcmp(body, other) == 0)
            return true;
    }
    return false;
}

/* Returns how many new keys came out of this file. */
static int load(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char line[KEY_MAX];
    int  added = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (!is_key_line(line))
            continue;
        if (nkeys >= MAX_KEYS) {
            dprintf(STDERR_FILENO,
                    "authkey: more than %d keys - the rest of %s is"
                    " ignored\n", MAX_KEYS, path);
            break;
        }
        if (already_have(line))
            continue;
        strlcpy(keys[nkeys++], line, KEY_MAX);
        added++;
    }

    lp_close((int)fd);
    return added;
}

/* Defined below, next to the other things this shells out to. */
static int run(const char *path, char *const argv[]);

/* ── telling integrity(1) that this was us ───────────────────────────
 *
 * integrity(1) hashes /root/.ssh/authorized_keys at every boot, because
 * a line added to it is how somebody who got in once gets in again. We
 * rewrite that file ourselves whenever a key is recovered from the boot
 * partition or the image, and that is most first boots - so integrity
 * announced "a file that survives a reboot has changed" at the boot
 * after every one of them, and defend(1) made it a security finding.
 * The one change on this machine that arrived through the documented,
 * intended route was the one being reported as an intrusion.
 *
 * So we hand integrity that one path, and only that one: `integrity -a`
 * re-records it and copies the rest of its record through untouched, so
 * an edit to /data/rc.local in the same window is still reported.
 *
 * What this does NOT do is touch defend's own baseline of authorized
 * keys. That baseline holds a fingerprint per key and is the check that
 * says WHICH key appeared; adopting a new key into it from here would
 * throw away the single most useful thing defend has to say. `authkey
 * add` prints the reminder instead - a person decides that one. */
static void integrity_accept_keys(void)
{
    if (!lp_exists("/bin/integrity"))
        return;                 /* an image without it watches nothing */

    char *argv[] = { (char *)"integrity", (char *)"-a",
                     (char *)LIVE, NULL };
    run("/bin/integrity", argv);
}

static bool write_live(void)
{
    /* The directory may not be there on a boot where /data did not
     * mount: /root is then whatever the image has. */
    lp_mkdir("/root", 0700);
    lp_mkdir("/root/.ssh", 0700);

    long fd = lp_open(LIVE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;

    for (int i = 0; i < nkeys; i++) {
        lp_write((int)fd, keys[i], strlen(keys[i]));
        lp_write((int)fd, "\n", 1);
    }
    lp_close((int)fd);

    /* Here rather than in each of the three callers: every route that
     * ends in this file being rewritten is one of ours, and a caller
     * added later would otherwise reintroduce the false report. */
    integrity_accept_keys();
    return true;
}

/* ── where the generated private key goes ─────────────────────────── */

#define PRIV_BOOT  "/boot/lp_ssh_key"
#define PRIV_DATA  "/data/lp_ssh_key"
#define HOSTKEY    "/data/lp_ssh_key.dropbear"

static int run(const char *path, char *const argv[])
{
    pid_t pid = lp_fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        extern char **environ;
        lp_execve(path, argv, environ);
        lp_exit(127);
    }
    int st = 0;
    lp_waitpid(pid, &st, 0);
    return LP_WIFEXITED(st) ? LP_WEXITSTATUS(st) : -1;
}

/* This machine's address, for the command line we print.
 *
 * Printing "ssh -i lp_ssh_key root@<the machine's address>" and leaving
 * the reader to find it is most of the work still undone. */
static bool my_address(char *out, size_t n)
{
    static const char *ifs[] = { "eth0", "wlan0", "eth1", "usb0", NULL };
    for (int i = 0; ifs[i]; i++) {
        u32 a = 0;
        if (net_get_addr(ifs[i], &a) == 0 && a != 0) {
            char buf[24];
            ipv4_format(a, buf);
            strlcpy(out, buf, n);
            return true;
        }
    }
    return false;
}

/* /boot is mounted read-only, deliberately: it is the way back in when
 * /data is gone, and a partition nothing writes to is one a power cut
 * cannot corrupt. Writing the private key there is worth the moment it
 * is writable, because a FAT partition is the one place a person can
 * read from any computer they own. Put it back immediately either way. */
static bool write_private_to_boot(const char *from)
{
    char *ro[]  = { (char *)"mount", (char *)"-o", (char *)"remount,rw",
                    (char *)"/boot", NULL };
    char *back[] = { (char *)"mount", (char *)"-o", (char *)"remount,ro",
                     (char *)"/boot", NULL };

    if (run("/bin/mount", ro) != 0)
        return false;

    char *cp[] = { (char *)"cp", (char *)from, (char *)PRIV_BOOT, NULL };
    bool ok = run("/bin/cp", cp) == 0;
    if (ok)
        lp_chmod(PRIV_BOOT, 0600);

    lp_sync();
    run("/bin/mount", back);
    return ok;
}

static int cmd_new(void)
{
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "authkey: only root can do this\n");
        return 1;
    }

    printf("authkey: making a key for this machine\n");

    /* dropbearkey is already here - it makes the host key at every first
     * boot - and it is the only key generator on the system. Its -y
     * prints the public half in the one line format authorized_keys
     * wants. */
    lp_unlink(HOSTKEY);
    char *gen[] = { (char *)"dropbearkey", (char *)"-t", (char *)"ed25519",
                    (char *)"-f", (char *)HOSTKEY, NULL };
    if (run("/bin/dropbearkey", gen) != 0) {
        dprintf(STDERR_FILENO,
                "authkey: dropbearkey could not make a key.\n"
                "authkey:   is /data mounted and writable? `df` says.\n");
        return 1;
    }
    lp_chmod(HOSTKEY, 0600);

    /* The public half, into authorized_keys. dropbearkey -y writes it to
     * standard output among other lines, so it is filtered by shape -
     * is_key_line already knows what one looks like. */
    long tmp = lp_open("/tmp/.authkey.pub", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tmp < 0) {
        dprintf(STDERR_FILENO, "authkey: cannot write to /tmp\n");
        return 1;
    }
    pid_t pid = lp_fork();
    if (pid == 0) {
        extern char **environ;
        lp_dup2((int)tmp, STDOUT_FILENO);
        char *y[] = { (char *)"dropbearkey", (char *)"-y",
                      (char *)"-f", (char *)HOSTKEY, NULL };
        lp_execve("/bin/dropbearkey", y, environ);
        lp_exit(127);
    }
    int st = 0;
    lp_waitpid(pid, &st, 0);
    lp_close((int)tmp);

    int found = load("/tmp/.authkey.pub");
    lp_unlink("/tmp/.authkey.pub");
    if (found <= 0) {
        dprintf(STDERR_FILENO,
                "authkey: dropbearkey made a key but printed no public"
                " half for it\n");
        return 1;
    }

    if (!write_live()) {
        dprintf(STDERR_FILENO,
                "authkey: cannot write %s - is /data writable?\n", LIVE);
        return 1;
    }

    /* Where the person picks the private half up. */
    const char *where = PRIV_BOOT;
    bool on_boot = write_private_to_boot(HOSTKEY);
    if (!on_boot) {
        char *cp[] = { (char *)"cp", (char *)HOSTKEY,
                       (char *)PRIV_DATA, NULL };
        if (run("/bin/cp", cp) != 0) {
            dprintf(STDERR_FILENO,
                    "authkey: the key works, but it could not be copied"
                    " anywhere you can reach it.\n"
                    "authkey:   it is at %s - copy that off yourself.\n",
                    HOSTKEY);
            where = HOSTKEY;
        } else {
            lp_chmod(PRIV_DATA, 0600);
            where = PRIV_DATA;
        }
    }

    char addr[32];
    bool have = my_address(addr, sizeof addr);

    printf("\n");
    printf("authkey: done. The key is authorized on this machine.\n\n");
    if (on_boot)
        printf("  The private half is %s, on the FAT boot partition -\n"
               "  put the card in any computer and copy it off, or fetch it\n"
               "  over this session.\n\n", PRIV_BOOT);
    else
        printf("  The private half is %s. /boot could not be written,\n"
               "  so copy it off from here.\n\n", where);

    printf("  Then, on your own machine:\n\n");
    printf("      chmod 600 lp_ssh_key\n");
    if (have)
        printf("      ssh -i lp_ssh_key root@%s\n\n", addr);
    else
        printf("      ssh -i lp_ssh_key root@<this machine>\n"
               "        (no address yet - `net` sets one up,"
               " `info net` shows it)\n\n");
    printf("  chmod 600 is not optional: ssh refuses a private key that\n"
           "  anyone else on your computer could read.\n");

    /* Only dropbear's own format is produced here. If it is not what
     * ssh wants, say so now rather than leaving somebody to find out. */
    printf("\n  The key is in dropbear's format. OpenSSH reads it; if\n"
           "  yours does not, `dropbearconvert dropbear openssh` on any\n"
           "  machine that has it will convert it.\n");

    /* Same reason as in `authkey add`: defend's list of keys allowed to
     * log in is a decision, not something a command takes on somebody's
     * behalf, so the person who just made a key is told how to record
     * it while they are still looking at the screen. */
    if (lp_exists("/bin/defend"))
        printf("\n  `defend baseline` records this key as expected;"
               " until then\n"
               "  defend reports it as a key it has not seen before.\n");
    return 0;
}

/* authkey add - from a file, or from a paste on standard input.
 *
 * Copying a key by hand into an editor is the other half of why this was
 * painful, and a key pasted with a line break in the middle of it is the
 * commonest way it goes wrong - so what arrives is checked for shape
 * before it is accepted, and refused with what was wrong. */
static int cmd_add(const char *file)
{
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "authkey: only root can do this\n");
        return 1;
    }

    load(IMAGE);
    load(BOOT);
    load(LIVE);
    int before = nkeys;

    int fd = STDIN_FILENO;
    if (file) {
        long f = lp_open(file, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "authkey: %s: cannot read it\n", file);
            return 1;
        }
        fd = (int)f;
    } else {
        printf("authkey: paste the public key - one line, starting"
               " ssh-ed25519 or ssh-rsa.\n");
        printf("authkey:   Ctrl-D when you are done.\n");
    }

    char line[KEY_MAX];
    int  bad = 0;
    while (readline(fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#')
            continue;
        if (!is_key_line(p)) {
            dprintf(STDERR_FILENO,
                    "authkey: this is not a public key line:\n"
                    "authkey:   %.60s\n"
                    "authkey:   It has to be one line beginning ssh-ed25519,"
                    " ssh-rsa or\n"
                    "authkey:   ecdsa-sha2-*. A key broken across lines by a"
                    " paste is the\n"
                    "authkey:   usual cause - join it back up.\n", p);
            bad++;
            continue;
        }
        if (already_have(p))
            continue;
        if (nkeys < MAX_KEYS)
            strlcpy(keys[nkeys++], p, KEY_MAX);
    }
    if (fd != STDIN_FILENO)
        lp_close(fd);

    if (nkeys == before) {
        if (bad)
            return 1;
        printf("authkey: nothing new - that key was already authorized\n");
        return 0;
    }

    if (!write_live()) {
        dprintf(STDERR_FILENO, "authkey: cannot write %s\n", LIVE);
        return 1;
    }

    char addr[32];
    printf("authkey: %d key%s added, %d authorized now.\n",
           nkeys - before, (nkeys - before) == 1 ? "" : "s", nkeys);
    if (my_address(addr, sizeof addr))
        printf("authkey:   ssh root@%s\n", addr);
    /* defend keeps its own list of the keys that are allowed to log in,
     * on purpose: a key nobody expected is the finding it exists for, so
     * nothing adopts one into that list on a person's behalf. Said here
     * because this is the moment the person is looking, and the
     * alternative is defend reporting their own key at every full scan
     * from now until somebody works out why. */
    if (lp_exists("/bin/defend"))
        printf("authkey:   `defend baseline` if you want defend to stop"
               " calling this key new\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    bool quiet = false, list_only = false;

    if (argc > 1 && strcmp(argv[1], "new") == 0)
        return cmd_new();
    if (argc > 1 && strcmp(argv[1], "add") == 0)
        return cmd_add(argc > 2 ? argv[2] : NULL);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = true;
        else if (strcmp(argv[i], "-l") == 0) list_only = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: authkey [-l] [-q]\n");
            printf("       authkey new          make a key here and print"
                   " how to connect\n");
            printf("       authkey add [file]   add a public key, from a"
                   " file or a paste\n\n");
            printf("  Merges the SSH keys from the boot partition and\n");
            printf("  from the system image into %s,\n", LIVE);
            printf("  so that losing the data partition does not lock\n");
            printf("  you out of your own machine.\n\n");
            printf("  -l  list what is authorized, and where it came from\n");
            printf("  -q  say nothing; the exit code is the answer\n\n");
            printf("  Exit 1 means no key is authorized - nobody can\n");
            printf("  log in over SSH, including you.\n");
            return 0;
        }
    }

    /* ── Recovery keys first, live keys after ──
     *
     * This used to be live, boot, image - and the table stops at
     * MAX_KEYS. The live file is on /data, which this program's own
     * header says to treat as lost or hostile, so anyone who could
     * write it could pad it to MAX_KEYS lines and the image key - the
     * one described here as impossible to remove without writing a new
     * card - was silently the first thing dropped. authkey then
     * reported "32 keys" and exited 0, and the documented recovery
     * route (put your key on the FAT partition and reboot) stopped
     * working, permanently, on a board with no Ethernet and no
     * passwords.
     *
     * The two keys that exist to get you back in are now loaded first
     * and cannot be crowded out. */
    int image = load(IMAGE);
    int boot  = load(BOOT);
    int live  = load(LIVE);

    if (list_only) {
        if (nkeys == 0) {
            printf("no keys authorized - nobody can log in over SSH.\n");
            printf("  `authkey new` makes one here and prints the command\n");
            printf("  to connect with it.\n");
            return 1;
        }
        printf("%d key%s authorized:\n", nkeys, nkeys == 1 ? "" : "s");
        for (int i = 0; i < nkeys; i++) {
            /* The tail of the line is usually who it belongs to. */
            printf("  %s\n", keys[i]);
        }
        return 0;
    }

    if (nkeys == 0) {
        if (!quiet) {
            dprintf(STDERR_FILENO,
                "authkey: ** no SSH key anywhere. Nobody can log in.\n"
                "authkey:\n"
                "authkey:    authkey new    makes one here and prints the\n"
                "authkey:                   command to connect with it\n"
                "authkey:    authkey add    if you already have a key to\n"
                "authkey:                   paste in\n"
                "authkey:\n"
                "authkey:    Or put your public key in authorized_keys on\n"
                "authkey:    the boot partition - it is FAT32, so any PC\n"
                "authkey:    can write it - and reboot.\n");
        }
        return 1;
    }

    /* Only write when something changed, so a healthy boot does not
     * touch the data partition at all. */
    if (boot > 0 || image > 0) {
        if (!write_live()) {
            if (!quiet)
                dprintf(STDERR_FILENO,
                        "authkey: cannot write %s\n"
                        "authkey:   the keys are in RAM for this boot only\n",
                        LIVE);
            return 0;           /* the keys are still usable this boot */
        }
        if (!quiet)
            printf("authkey: %d key%s (%d recovered from the boot partition,"
                   " %d from the system image)\n",
                   nkeys, nkeys == 1 ? "" : "s", boot, image);
    } else if (!quiet) {
        printf("authkey: %d key%s\n", live, live == 1 ? "" : "s");
    }

    return 0;
}
