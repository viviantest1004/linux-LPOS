/* dd - copy blocks, exactly as told.
 *
 *   dd if=<file> of=<file> [bs=N] [count=N] [skip=N] [seek=N] [conv=...]
 *
 *   if=    where to read (default: standard input)
 *   of=    where to write (default: standard output)
 *   bs=    block size, with K, M or G  (default 512)
 *   count= how many blocks, not all of them
 *   skip=  skip this many blocks of the input first
 *   seek=  skip this many blocks of the output first
 *   conv=  notrunc  do not shorten the output file
 *          sync     pad a short read out to a full block with zeros
 *          fsync    flush to the device before finishing
 *   status=none    say nothing at the end
 *
 * The instructions for this system tell people to burn a card with dd
 * on their own computer, and there was no dd on the machine itself -
 * which meant no way to make a file of a given size, no way to zero the
 * front of a disk before `part` writes to it, and no way to take a raw
 * copy of a partition. `cp` cannot do any of those: it stops at the end
 * of a file, and a block device has no end it can see.
 *
 * The odd syntax is not a style choice. dd has taken key=value since
 * 1970, every instruction written anywhere uses it, and a dd here that
 * wanted -i and -o would silently do the wrong thing with a command
 * somebody pasted. So it is the old syntax, and an argument that is not
 * key=value is refused by name rather than guessed at.
 *
 * ── The one that destroys things ──
 * of=/dev/sda with the wrong letter is the mistake on this machine that
 * a reboot cannot undo. So writing to a block device asks first, unless
 * the output is a plain file - which is the case for every use that is
 * not dangerous.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool parse_size(const char *s, u64 *out)
{
    char *end;
    /* strtoll: a long is four bytes on a 32-bit machine, so
     * "skip=3000" with bs=1M parsed as a negative number there. */
    s64 v = strtoll(s, &end, 10);
    if (end == s || v < 0)
        return false;

    u64 n = (u64)v;
    if (*end) {
        /* One suffix, and only the three anybody types. "1MB" and "1M"
         * mean the same thing here; GNU dd's MB-is-1000000 distinction
         * is a trap more often than it is useful. */
        char c = *end++;
        if (c == 'k' || c == 'K')      n *= 1024;
        else if (c == 'm' || c == 'M') n *= 1024ULL * 1024;
        else if (c == 'g' || c == 'G') n *= 1024ULL * 1024 * 1024;
        else return false;
        if (*end == 'B' || *end == 'b') end++;
        if (*end) return false;
    }
    *out = n;
    return true;
}

static bool is_block_device(const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, true) < 0)
        return false;
    return (st.mode & LP_S_IFMT) == LP_S_IFBLK;
}

static void human(u64 bytes, char *buf, size_t n)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, n, "%lu.%lu GB", (unsigned long)(bytes >> 30),
                 (unsigned long)(((bytes >> 20) % 1024) * 10 / 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, n, "%lu MB", (unsigned long)(bytes >> 20));
    else if (bytes >= 1024)
        snprintf(buf, n, "%lu KB", (unsigned long)(bytes >> 10));
    else
        snprintf(buf, n, "%lu bytes", (unsigned long)bytes);
}

int main(int argc, char **argv)
{
    const char *in_name = NULL, *out_name = NULL;
    u64  bs = 512, count = 0, skip = 0, seek = 0;
    bool have_count = false, notrunc = false, do_sync = false;
    bool pad = false, quiet = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            printf("usage: dd if=<file> of=<file> [bs=N] [count=N]"
                   " [skip=N] [seek=N]\n");
            printf("  bs= takes K, M or G.  conv=notrunc,sync,fsync\n");
            printf("  make a 100MB file:   dd if=/dev/zero of=big bs=1M count=100\n");
            printf("  copy a partition:    dd if=/dev/sda2 of=/data/part.img bs=4M\n");
            printf("  writing to a block device asks first\n");
            return 0;
        }

        const char *eq = strchr(a, '=');
        if (!eq) {
            dprintf(STDERR_FILENO,
                    "dd: \"%s\" is not key=value. dd has taken key=value since\n"
                    "dd:   1970 and every instruction anywhere is written that\n"
                    "dd:   way, so this does not guess. `dd -h` shows the keys.\n",
                    a);
            return 2;
        }

        size_t klen = (size_t)(eq - a);
        const char *v = eq + 1;

        if (klen == 2 && strncmp(a, "if", 2) == 0)        in_name = v;
        else if (klen == 2 && strncmp(a, "of", 2) == 0)   out_name = v;
        else if (klen == 2 && strncmp(a, "bs", 2) == 0) {
            if (!parse_size(v, &bs) || bs == 0) {
                dprintf(STDERR_FILENO, "dd: bs=%s is not a size\n", v);
                return 2;
            }
        }
        else if (klen == 5 && strncmp(a, "count", 5) == 0) {
            if (!parse_size(v, &count)) {
                dprintf(STDERR_FILENO, "dd: count=%s is not a number\n", v);
                return 2;
            }
            have_count = true;
        }
        else if (klen == 4 && strncmp(a, "skip", 4) == 0) {
            if (!parse_size(v, &skip)) {
                dprintf(STDERR_FILENO, "dd: skip=%s is not a number\n", v);
                return 2;
            }
        }
        else if (klen == 4 && strncmp(a, "seek", 4) == 0) {
            if (!parse_size(v, &seek)) {
                dprintf(STDERR_FILENO, "dd: seek=%s is not a number\n", v);
                return 2;
            }
        }
        else if (klen == 6 && strncmp(a, "status", 6) == 0) {
            quiet = (strcmp(v, "none") == 0);
        }
        else if (klen == 4 && strncmp(a, "conv", 4) == 0) {
            /* A comma-separated list, and anything in it we do not know
             * is refused: silently ignoring conv=noerror would copy a
             * failing disk and report success. */
            char buf[128];
            strlcpy(buf, v, sizeof buf);
            char *p = buf;
            while (*p) {
                char *comma = strchr(p, ',');
                if (comma) *comma = '\0';
                if (strcmp(p, "notrunc") == 0)    notrunc = true;
                else if (strcmp(p, "sync") == 0)  pad = true;
                else if (strcmp(p, "fsync") == 0) do_sync = true;
                else {
                    dprintf(STDERR_FILENO,
                            "dd: conv=%s is not one this dd does."
                            " It knows notrunc, sync and fsync.\n", p);
                    return 2;
                }
                if (!comma) break;
                p = comma + 1;
            }
        }
        else {
            dprintf(STDERR_FILENO, "dd: no such key: %.*s\n", (int)klen, a);
            return 2;
        }
    }

    if (bs > 64ULL * 1024 * 1024) {
        dprintf(STDERR_FILENO,
                "dd: bs=%luM is larger than this will allocate."
                " 4M is already as fast as it gets.\n",
                (unsigned long)(bs >> 20));
        return 2;
    }

    /* The one that cannot be undone. */
    if (out_name && is_block_device(out_name)) {
        char what[32];
        u64  size = 0;
        lp_stat_t st;
        (void)st;
        printf("dd: %s is a disk, not a file. Writing to it destroys"
               " whatever is on it.\n", out_name);
        if (lp_fs_space(out_name, &size, &size) != 0)
            size = 0;
        human(size, what, sizeof what);
        printf("dd:   `disk` lists what each one is."
               " Type yes to go ahead: ");
        char answer[16];
        long n = lp_read(STDIN_FILENO, answer, sizeof answer - 1);
        if (n <= 0) return 1;
        answer[n] = '\0';
        for (long i = 0; i < n; i++)
            if (answer[i] == '\n' || answer[i] == '\r') answer[i] = '\0';
        if (strcmp(answer, "yes") != 0) {
            printf("dd: nothing was written\n");
            return 1;
        }
    }

    int  in  = STDIN_FILENO;
    int  out = STDOUT_FILENO;

    if (in_name) {
        long f = lp_open(in_name, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "dd: %s: cannot read it\n", in_name);
            return 1;
        }
        in = (int)f;
    }
    if (out_name) {
        int flags = O_WRONLY | O_CREAT | (notrunc ? 0 : O_TRUNC);
        long f = lp_open(out_name, flags, 0644);
        if (f < 0) {
            dprintf(STDERR_FILENO, "dd: %s: cannot write to it\n", out_name);
            if (in != STDIN_FILENO) lp_close(in);
            return 1;
        }
        out = (int)f;
    }

    if (skip && lp_lseek(in, (off_t)(skip * bs), SEEK_SET) < 0) {
        dprintf(STDERR_FILENO,
                "dd: cannot skip %lu blocks - is the input a pipe?\n",
                (unsigned long)skip);
        return 1;
    }
    if (seek && lp_lseek(out, (off_t)(seek * bs), SEEK_SET) < 0) {
        dprintf(STDERR_FILENO, "dd: cannot seek %lu blocks into the output\n",
                (unsigned long)seek);
        return 1;
    }

    char *buf = malloc(bs);
    if (!buf) {
        dprintf(STDERR_FILENO,
                "dd: no memory for a %lu byte block. Use a smaller bs=.\n",
                (unsigned long)bs);
        return 1;
    }

    u64  full = 0, partial = 0, bytes = 0;
    int  rc = 0;

    for (u64 done = 0; !have_count || done < count; done++) {
        long n = lp_read(in, buf, bs);
        if (n < 0) {
            dprintf(STDERR_FILENO,
                    "dd: read failed after %lu bytes."
                    " On a disk that usually means a bad sector.\n",
                    (unsigned long)bytes);
            rc = 1;
            break;
        }
        if (n == 0)
            break;

        if ((u64)n < bs) {
            partial++;
            if (pad) { memset(buf + n, 0, bs - (size_t)n); n = (long)bs; }
        } else {
            full++;
        }

        long w = lp_write(out, buf, (size_t)n);
        if (w != n) {
            dprintf(STDERR_FILENO,
                    "dd: wrote %ld of %ld bytes after %lu."
                    " The destination is probably full.\n",
                    w < 0 ? 0 : w, n, (unsigned long)bytes);
            rc = 1;
            break;
        }
        bytes += (u64)n;
    }

    if (do_sync)
        lp_sync();

    free(buf);
    if (in  != STDIN_FILENO)  lp_close(in);
    if (out != STDOUT_FILENO) lp_close(out);

    if (!quiet) {
        char h[32];
        human(bytes, h, sizeof h);
        dprintf(STDERR_FILENO, "%lu+%lu blocks, %s\n",
                (unsigned long)full, (unsigned long)partial, h);
    }
    return rc;
}
