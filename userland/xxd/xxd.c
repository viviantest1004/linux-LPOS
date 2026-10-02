/* xxd - look at the bytes, and put them back again.
 *
 *   xxd file            offset, hex, and the printable characters
 *   xxd -p file         nothing but the hex
 *   xxd file | xxd -r   back to the original bytes
 *
 * This one is not from coreutils. It comes with vim, and its habits are
 * vim's: an option is matched by its first two characters, so "-ps" is
 * "-p" with the s ignored, and a second operand is an output file rather
 * than a second input. That is why the option loop below is written out
 * by hand instead of using lp_getopt - the grammar really is different,
 * and somebody who knows xxd would be tripped up by the GNU one.
 *
 * The point of -r is patching. A dump can be edited in a text editor and
 * poured back in, and only the lines you touched need be there: each
 * line carries its own offset, so -r seeks to it. That is also why -r
 * throws away whatever follows the hex on a line - the ASCII column is
 * there for you to read, not for it.
 *
 * The dump line is built by writing into a buffer already full of spaces,
 * at positions worked out from the column, rather than by printing one
 * field after another. It looks roundabout until the last line of a file
 * comes out short and still lines up with the ones above it.
 *
 * Not here, and why:
 *   -b -e -E   binary, little-endian and EBCDIC dumps; each is a whole
 *              third format to keep exactly right and nothing on this
 *              machine has asked for one
 *   -R         colour. The real xxd colours its output when stdout is a
 *              terminal; this one never does
 *   -v         the version banner would have to name a vim release that
 *              this file is not part of
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PROG "xxd"
#define COLS 256
#define LLEN (16 + 4 + (9 * COLS - 1) + COLS + 2)

static const char *hexx = "0123456789abcdef";
static bool upper_hex = false;

static int fdi = STDIN_FILENO, fdo = STDOUT_FILENO;

static void perror_exit(int code, const char *what, int err)
    __attribute__((noreturn));
static void perror_exit(int code, const char *what, int err)
{
    /* xxd names the file only where it has one to hand; a read that
     * fails halfway through reports the error alone. */
    if (what)
        dprintf(STDERR_FILENO, PROG ": %s: %s\n", what, lp_strerror(err));
    else
        dprintf(STDERR_FILENO, PROG ": %s\n", lp_strerror(err));
    lp_exit(code);
}

static void error_exit(int code, const char *msg) __attribute__((noreturn));
static void error_exit(int code, const char *msg)
{
    dprintf(STDERR_FILENO, PROG ": %s\n", msg);
    lp_exit(code);
}

/* ── output, buffered, and always to fdo so that an outfile works ── */
static u8     obuf[65536];
static size_t olen;

static void oflush(void)
{
    size_t off = 0;
    while (off < olen) {
        long w = lp_write(fdo, obuf + off, olen - off);
        if (w <= 0) perror_exit(3, NULL, w < 0 ? (int)-w : 0);
        off += (size_t)w;
    }
    olen = 0;
}

static void oputc(int c)
{
    if (olen == sizeof obuf) oflush();
    obuf[olen++] = (u8)c;
}

static void oputs(const char *s)
{
    while (*s) oputc((unsigned char)*s++);
}

/* ── input, buffered ── */
static u8     ibuf[65536];
static size_t ilen, ipos;

static int igetc(void)
{
    if (ipos < ilen) return ibuf[ipos++];
    long n = lp_read(fdi, ibuf, sizeof ibuf);
    if (n < 0) perror_exit(2, NULL, (int)-n);
    if (n == 0) return -1;
    ilen = (size_t)n; ipos = 0;
    return ibuf[ipos++];
}

static void usage(void) __attribute__((noreturn));
static void usage(void)
{
    dprintf(STDERR_FILENO,
        "Usage:\n"
        "       xxd [options] [infile [outfile]]\n"
        "    or\n"
        "       xxd -r [-s [-]offset] [-c cols] [-ps] [infile [outfile]]\n"
        "Options:\n"
        "    -a          toggle autoskip: A single '*' replaces nul-lines."
                                                        " Default off.\n"
        "    -C          capitalize variable names in C include file style (-i).\n"
        "    -c cols     format <cols> octets per line. Default 16"
                                                        " (-i: 12, -ps: 30).\n"
        "    -d          show offset in decimal instead of hex.\n"
        "    -g bytes    number of octets per group in normal output. Default 2.\n"
        "    -h          print this summary.\n"
        "    -i          output in C include file style.\n"
        "    -l len      stop after <len> octets.\n"
        "    -n name     set the variable name used in C include output (-i).\n"
        "    -o off      add <off> to the displayed file position.\n"
        "    -ps         output in postscript plain hexdump style.\n"
        "    -r          reverse operation: convert (or patch) hexdump into binary.\n"
        "    -r -s off   revert with <off> added to file positions found in hexdump.\n"
        "    -s [+][-]seek  start at <seek> bytes abs. (or +: rel.) infile offset.\n"
        "    -u          use upper case hex letters.\n");
    lp_exit(1);
}

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_alnum(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ── -r: hex back into bytes ──
 *
 * Two characters in a row that are not hex end the data on a line and
 * the rest of it is thrown away, which is what lets a whole dump -
 * ASCII column and all - be fed straight back in. */
static int huntype(int cols, bool postscript, s64 base_off)
{
    int  c, n1 = -1, n2 = 0, n3 = 0;
    bool ign_garb = true;
    int  p = cols;
    s64  have_off = 0, want_off = 0;

    while ((c = igetc()) >= 0) {
        if (c == '\r')
            continue;
        if (postscript && (c == ' ' || c == '\n' || c == '\t'))
            continue;

        n3 = n2;
        n2 = n1;
        n1 = hexval(c);
        if (n1 == -1 && ign_garb)
            continue;
        ign_garb = false;

        if (!postscript && p >= cols) {
            /* Still reading the address at the head of the line; the
             * first non-hex character (the colon) ends it. */
            if (n1 < 0) { p = 0; continue; }
            want_off = (want_off << 4) | n1;
            continue;
        }

        if (base_off + want_off != have_off) {
            s64 target = base_off + want_off;
            oflush();
            s64 got = lp_lseek(fdo, target - have_off, SEEK_CUR);
            if (got >= 0)
                have_off = target;
            if (target < have_off)
                error_exit(5, "Sorry, cannot seek backwards.");
            /* Output that will not seek, and the gap is forwards: fill
             * it with zeros, which is what a pipe gets. */
            for (; have_off < target; have_off++)
                oputc(0);
        }

        if (n2 >= 0 && n1 >= 0) {
            oputc((n2 << 4) | n1);
            have_off++;
            want_off++;
            n1 = -1;
            if (!postscript && ++p >= cols)
                while (c != '\n' && c >= 0) c = igetc();
        } else if (n1 < 0 && n2 < 0 && n3 < 0) {
            while (c != '\n' && c >= 0) c = igetc();
        }

        if (c == '\n') {
            if (!postscript) want_off = 0;
            p = cols;
            ign_garb = true;
        }
    }
    oflush();
    lp_lseek(fdo, 0, SEEK_END);
    return 0;
}

/* ── autoskip ──
 * A line of nothing but zeros is held back; three or more in a row
 * collapse to one '*'. nz < 0 means "the input ended here, flush". */
static char zline[LLEN + 1];
static int  zero_seen = 0;

static void xxdline(const char *l, int nz)
{
    if (!nz && zero_seen == 1)
        strcpy(zline, l);

    if (nz || !zero_seen++) {
        if (nz) {
            if (nz < 0) zero_seen--;
            if (zero_seen == 2) oputs(zline);
            if (zero_seen > 2)  oputs("*\n");
        }
        if (nz >= 0 || zero_seen > 0)
            oputs(l);
        if (nz) zero_seen = 0;
    }
}

/* The name -i gives the array: the file name with everything that is not
 * a letter or a digit turned into an underscore. A leading digit would
 * not be a C identifier, so two underscores go in front of it. */
static void put_varname(const char *v, bool capitalize)
{
    if (is_digit((unsigned char)v[0]))
        oputs("__");
    for (; *v; v++) {
        int ch = (unsigned char)*v;
        if (!is_alnum(ch))                            ch = '_';
        else if (capitalize && ch >= 'a' && ch <= 'z') ch -= 32;
        oputc(ch);
    }
}

int main(int argc, char **argv)
{
    bool autoskip = false, revert = false, capitalize = false;
    bool decimal_offset = false, postscript = false, cinclude = false;
    bool colsgiven = false;
    int  cols = 0, octspergrp = -1;
    s64  length = -1, seekoff = 0;
    u64  displayoff = 0;
    int  relseek = 1, negseek = 0;
    const char *varname = NULL, *iname = NULL, *oname = NULL;

    int i = 1;
    while (i < argc) {
        char *pp = argv[i];
        if (strncmp(pp, "--", 2) == 0 && pp[2])
            pp++;                       /* "--foo" is read as "-foo" */

        const char *attached = pp[2] ? pp + 2 : NULL;
        const char *value;

        if      (strncmp(pp, "-a", 2) == 0) autoskip = !autoskip;
        else if (strncmp(pp, "-u", 2) == 0) { upper_hex = true;
                                              hexx = "0123456789ABCDEF"; }
        else if (strncmp(pp, "-p", 2) == 0) { postscript = true; }
        else if (strncmp(pp, "-i", 2) == 0) { cinclude = true; }
        else if (strncmp(pp, "-C", 2) == 0) capitalize = true;
        else if (strncmp(pp, "-d", 2) == 0) decimal_offset = true;
        else if (strncmp(pp, "-r", 2) == 0) revert = true;
        else if (strncmp(pp, "-c", 2) == 0) {
            if (attached && strncmp("apitalize", attached, 9) == 0) {
                capitalize = true;
            } else {
                if (attached && strncmp("ols", attached, 3) != 0) value = attached;
                else if (i + 1 < argc) value = argv[++i];
                else usage();
                colsgiven = true;
                cols = (int)strtol(value, NULL, 0);
            }
        }
        else if (strncmp(pp, "-g", 2) == 0) {
            if (attached && strncmp("roup", attached, 4) != 0) value = attached;
            else if (i + 1 < argc) value = argv[++i];
            else usage();
            octspergrp = (int)strtol(value, NULL, 0);
        }
        else if (strncmp(pp, "-o", 2) == 0) {
            if (attached && strncmp("ffset", attached, 5) != 0) {
                displayoff = (u64)strtoll(attached, NULL, 0);
            } else {
                int rel = 0, neg = 0;
                if (i + 1 >= argc) usage();
                value = argv[++i];
                if (value[0] == '+') rel++;
                if (value[rel] == '-') neg++;
                u64 v = (u64)strtol(value + rel + neg, NULL, 0);
                displayoff = neg ? (u64)0 - v : v;
            }
        }
        else if (strncmp(pp, "-s", 2) == 0) {
            relseek = 0; negseek = 0;
            if (attached && strncmp("kip", attached, 3) != 0 &&
                            strncmp("eek", attached, 3) != 0)
                value = attached;
            else if (i + 1 < argc) value = argv[++i];
            else usage();
            if (value[0] == '+') relseek++;
            if (value[relseek] == '-') negseek++;
            seekoff = strtoll(value + relseek + negseek, NULL, 0);
        }
        else if (strncmp(pp, "-l", 2) == 0) {
            if (attached && strncmp("en", attached, 2) != 0) value = attached;
            else if (i + 1 < argc) value = argv[++i];
            else usage();
            length = strtoll(value, NULL, 0);
        }
        else if (strncmp(pp, "-n", 2) == 0) {
            if (attached && strncmp("ame", attached, 3) != 0) value = attached;
            else if (i + 1 < argc) value = argv[++i];
            else usage();
            varname = value;
        }
        else if (strcmp(pp, "--") == 0) { i++; break; }
        else if (pp[0] == '-' && pp[1]) usage();
        else break;

        i++;
    }

    if (!colsgiven || (!cols && !postscript))
        cols = postscript ? 30 : cinclude ? 12 : 16;
    if (octspergrp < 0)
        octspergrp = (postscript || cinclude) ? 0 : 2;

    if ((postscript && cols < 0) || (!postscript && cols < 1) ||
        (!postscript && !cinclude && cols > COLS)) {
        dprintf(STDERR_FILENO,
                PROG ": invalid number of columns (max. %d).\n", COLS);
        return 1;
    }
    if (octspergrp < 1 || octspergrp > cols)
        octspergrp = cols;

    if (argc - i > 2)
        usage();
    if (i < argc && !(argv[i][0] == '-' && !argv[i][1]))
        iname = argv[i];
    if (i + 1 < argc && !(argv[i + 1][0] == '-' && !argv[i + 1][1]))
        oname = argv[i + 1];

    if (iname) {
        long fd = lp_open(iname, O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, PROG ": %s: %s\n",
                    iname, lp_strerror((int)-fd));
            return 2;
        }
        fdi = (int)fd;
    }
    if (oname) {
        /* Reverting patches a file that is already there, so it must not
         * be truncated - the offsets in the dump are the whole point. */
        int flags = O_WRONLY | O_CREAT | (revert ? 0 : O_TRUNC);
        long fd = lp_open(oname, flags, 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, PROG ": %s: %s\n",
                    oname, lp_strerror((int)-fd));
            return 3;
        }
        fdo = (int)fd;
    }

    if (revert) {
        if (cinclude)
            error_exit(255, "Sorry, cannot revert this type of hexdump");
        return huntype(cols, postscript, negseek ? -seekoff : seekoff);
    }

    if (seekoff || negseek || !relseek) {
        s64 e;
        if (relseek)
            e = lp_lseek(fdi, negseek ? -seekoff : seekoff, SEEK_CUR);
        else
            e = lp_lseek(fdi, negseek ? -seekoff : seekoff,
                         negseek ? SEEK_END : SEEK_SET);
        if (e < 0 && negseek)
            error_exit(4, "Sorry, cannot seek.");
        if (e >= 0)
            seekoff = e;
        else
            for (s64 s = seekoff; s--; )     /* a pipe: read past it */
                if (igetc() < 0)
                    error_exit(4, "Sorry, cannot seek.");
    }

    if (cinclude) {
        /* Naming the array after the file is why `xxd -i logo.png` gives
         * something that compiles as it stands. */
        if (varname == NULL && iname != NULL)
            varname = iname;

        if (varname) {
            oputs("unsigned char ");
            put_varname(varname, capitalize);
            oputs("[] = {\n");
        }

        s64 p = 0;
        int c;
        char num[16];
        while ((length < 0 || p < length) && (c = igetc()) >= 0) {
            oputs((p % cols) ? ", " : (!p ? "  " : ",\n  "));
            if (upper_hex) snprintf(num, sizeof num, "0X%02X", c);
            else           snprintf(num, sizeof num, "0x%02x", c);
            oputs(num);
            p++;
        }
        if (p) oputs("\n");

        if (varname) {
            oputs("};\nunsigned int ");
            put_varname(varname, capitalize);
            snprintf(num, sizeof num, "%ld", (long)p);
            oputs(capitalize ? "_LEN = " : "_len = ");
            oputs(num);
            oputs(";\n");
        }
        oflush();
        return 0;
    }

    if (postscript) {
        int p = cols, e;
        s64 n = 0;
        while ((length < 0 || n < length) && (e = igetc()) >= 0) {
            oputc(hexx[(e >> 4) & 0xf]);
            oputc(hexx[e & 0xf]);
            n++;
            if (cols > 0 && !--p) { oputc('\n'); p = cols; }
        }
        if (cols == 0 || p < cols)
            oputc('\n');
        oflush();
        return 0;
    }

    static char l[LLEN + 1];
    int  nonzero = 0;
    int  grplen = 2 * octspergrp + 1;
    int  addrlen = 9, p = 0, c = 0, e;
    s64  n = 0;

    while ((length < 0 || n < length) && (e = igetc()) >= 0) {
        if (p == 0) {
            unsigned long at = (unsigned long)((u64)(n + seekoff) + displayoff);
            addrlen = decimal_offset
                      ? snprintf(l, sizeof l, "%08lu:", at)
                      : snprintf(l, sizeof l, "%08lx:", at);
            for (int k = addrlen; k < LLEN; k++) l[k] = ' ';
        }
        c = addrlen + 1 + (grplen * p) / octspergrp;
        l[c]   = hexx[(e >> 4) & 0xf];
        l[++c] = hexx[e & 0xf];
        if (e) nonzero++;

        c = (grplen * cols - 1) / octspergrp + addrlen + 3 + p;
        l[c++] = (e > 31 && e < 127) ? (char)e : '.';
        n++;
        if (++p == cols) {
            l[c++] = '\n';
            l[c] = '\0';
            xxdline(l, autoskip ? nonzero : 1);
            nonzero = 0;
            p = 0;
        }
    }
    if (p) {
        l[c++] = '\n';
        l[c] = '\0';
        xxdline(l, 1);
    } else if (autoskip) {
        xxdline(l, -1);      /* the file ended inside a run of zeros */
    }
    oflush();
    return 0;
}
