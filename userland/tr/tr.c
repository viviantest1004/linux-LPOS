/* tr - replace, squeeze or delete characters.
 *
 *   tr abc xyz          a->x, b->y, c->z
 *   tr a-z A-Z          ranges
 *   tr -d '\r'          delete them
 *   tr -s ' '           collapse runs of them into one
 *   tr -c a-z .         everything that is NOT in the first set
 *
 * Escapes understood in a set: \n \r \t \\ \0 and \nnn in octal.
 * Classes: [:alpha:] [:digit:] [:alnum:] [:space:] [:upper:] [:lower:]
 * [:punct:].
 *
 * Bytes, not characters. tr has always worked on bytes, and the two
 * things people actually use it for here - stripping the \r out of a
 * file written on Windows and changing case in ASCII - are byte
 * operations. Feeding it a range of Hangul would not do what you meant.
 * GNU says nothing about that, so under its voice neither do we; under
 * `voice lp` there is a line about it, because it is the kind of thing
 * that costs an hour when nobody mentions it.
 *
 * What is not here: [c*n] repeats, [=c=] equivalence classes, and the
 * multibyte behaviour of a locale. The character classes that are here
 * ([:alpha:] and friends) are the ASCII ones.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static bool warned_utf8 = false;

/* Expand a set specification into a list of bytes. */
static int expand(const char *spec, unsigned char *out, int max)
{
    int n = 0;
    const char *p = spec;

    while (*p && n < max) {
        /* [:class:] */
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            if (end) {
                char name[16];
                size_t len = (size_t)(end - (p + 2));
                if (len < sizeof name) {
                    memcpy(name, p + 2, len);
                    name[len] = '\0';
                    for (int c = 0; c < 256 && n < max; c++) {
                        bool in = false;
                        if (strcmp(name, "alpha") == 0)
                            in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                        else if (strcmp(name, "digit") == 0)
                            in = (c >= '0' && c <= '9');
                        else if (strcmp(name, "alnum") == 0)
                            in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                              || (c >= '0' && c <= '9');
                        else if (strcmp(name, "space") == 0)
                            in = (c == ' ' || c == '\t' || c == '\n' ||
                                  c == '\r' || c == '\f' || c == '\v');
                        else if (strcmp(name, "upper") == 0)
                            in = (c >= 'A' && c <= 'Z');
                        else if (strcmp(name, "lower") == 0)
                            in = (c >= 'a' && c <= 'z');
                        else if (strcmp(name, "punct") == 0)
                            in = (c > 32 && c < 127) &&
                                 !((c >= 'a' && c <= 'z') ||
                                   (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'));
                        if (in) out[n++] = (unsigned char)c;
                    }
                    p = end + 2;
                    continue;
                }
            }
        }

        /* one byte, possibly escaped */
        unsigned char c;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'f': c = '\f'; p++; break;
            case 'v': c = '\v'; p++; break;
            case '\\': c = '\\'; p++; break;
            default:
                if (*p >= '0' && *p <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                    c = (unsigned char)v;
                } else c = (unsigned char)*p++;
            }
        } else {
            if ((unsigned char)*p >= 0x80 && !warned_utf8 &&
                lp_voice() == LP_VOICE_LP) {
                dprintf(STDERR_FILENO,
                        "tr: this works on bytes, and \"%s\" is not ASCII -\n"
                        "tr:   the result will not be what you meant. Use sed\n"
                        "tr:   for anything beyond ASCII.\n", spec);
                warned_utf8 = true;
            }
            c = (unsigned char)*p++;
        }

        /* a-z */
        if (*p == '-' && p[1] && p[1] != '-') {
            unsigned char hi;
            p++;
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': hi = '\n'; p++; break;
                case 't': hi = '\t'; p++; break;
                case 'r': hi = '\r'; p++; break;
                default:  hi = (unsigned char)*p++;
                }
            } else hi = (unsigned char)*p++;

            if (hi < c) {
                dprintf(STDERR_FILENO,
                        "tr: range-endpoints of '%c-%c' are in reverse "
                        "collating sequence order\n", c, hi);
                return -1;
            }
            for (int v = c; v <= hi && n < max; v++) out[n++] = (unsigned char)v;
            continue;
        }

        out[n++] = c;
    }
    return n;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: tr [OPTION]... STRING1 [STRING2]\n"
                "Translate, squeeze, and/or delete characters from standard input,\n"
                "writing to standard output.  STRING1 and STRING2 specify arrays of\n"
                "characters ARRAY1 and ARRAY2 that control the action.\n\n"
                "  -c, -C, --complement    use the complement of ARRAY1\n"
                "  -d, --delete            delete characters in ARRAY1, do not translate\n"
                "  -s, --squeeze-repeats   replace each sequence of a repeated character\n"
                "                            that is listed in the last specified ARRAY,\n"
                "                            with a single occurrence of that character\n"
                "  -t, --truncate-set1     first truncate ARRAY1 to length of ARRAY2\n"
                "      --help        display this help and exit\n\n"
                "ARRAYs are specified as strings of characters.  Most represent themselves.\n"
                "Interpreted sequences are:\n\n"
                "  \\NNN            character with octal value NNN (1 to 3 octal digits)\n"
                "  \\\\              backslash\n"
                "  \\a              audible BEL\n"
                "  \\b              backspace\n"
                "  \\f              form feed\n"
                "  \\n              new line\n"
                "  \\r              return\n"
                "  \\t              horizontal tab\n"
                "  \\v              vertical tab\n"
                "  CHAR1-CHAR2     all characters from CHAR1 to CHAR2 in ascending order\n"
                "  [:alnum:]       all letters and digits\n"
                "  [:alpha:]       all letters\n"
                "  [:digit:]       all digits\n"
                "  [:lower:]       all lower case letters\n"
                "  [:punct:]       all punctuation characters\n"
                "  [:space:]       all horizontal or vertical whitespace\n"
                "  [:upper:]       all upper case letters\n");
}

static void try_help(void)
{
    dprintf(STDERR_FILENO, "Try 'tr --help' for more information.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "complement", 0, 'c' }, { "delete", 0, 'd' },
        { "squeeze-repeats", 0, 's' }, { "truncate-set1", 0, 't' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool del = false, squeeze = false, complement = false, truncate1 = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "cCdst", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'c': case 'C': complement = true; break;
        case 'd': del = true; break;
        case 's': squeeze = true; break;
        case 't': truncate1 = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("tr", &g); return 1;
        }

    int i = g.ind, nops = argc - g.ind;
    if (nops == 0) {
        dprintf(STDERR_FILENO, "tr: missing operand\n");
        try_help();
        return 1;
    }
    /* Which operand is "extra" depends on what the other flags asked
     * for, and GNU explains the delete case because it is the one people
     * get wrong: `tr -d a b` looks like it should work. */
    if (nops > 2) {
        dprintf(STDERR_FILENO, "tr: extra operand '%s'\n", argv[i + 2]);
        try_help();
        return 1;
    }
    if (nops == 2 && del && !squeeze) {
        dprintf(STDERR_FILENO, "tr: extra operand '%s'\n"
                "Only one string may be given when deleting without "
                "squeezing repeats.\n", argv[i + 1]);
        try_help();
        return 1;
    }
    if (nops == 1 && !del && !squeeze) {
        dprintf(STDERR_FILENO, "tr: missing operand after '%s'\n"
                "Two strings must be given when translating.\n", argv[i]);
        try_help();
        return 1;
    }

    unsigned char s1[512], s2[512];
    int n1 = expand(argv[i], s1, sizeof s1);
    if (n1 < 0) return 1;
    int n2 = 0;
    if (nops > 1) {
        n2 = expand(argv[i + 1], s2, sizeof s2);
        if (n2 < 0) return 1;
    }

    /* Build the tables once: 256 entries beats searching a list per byte
     * on a 1GHz core. */
    bool in_set1[256], in_set2[256];
    unsigned char map[256];
    for (int c = 0; c < 256; c++) {
        in_set1[c] = false;
        in_set2[c] = false;
        map[c] = (unsigned char)c;
    }
    for (int k = 0; k < n1; k++) in_set1[s1[k]] = true;
    if (complement)
        for (int c = 0; c < 256; c++) in_set1[c] = !in_set1[c];
    for (int k = 0; k < n2; k++) in_set2[s2[k]] = true;

    if (!del && n2 > 0) {
        /* A short second set repeats its last character, which is what
         * makes `tr a-z x` turn every letter into x. -t says to drop the
         * tail of set1 instead, so those bytes come through unchanged. */
        int k = 0;
        for (int c = 0; c < 256; c++) {
            if (!in_set1[c]) continue;
            if (k < n2)          map[c] = s2[k];
            else if (!truncate1) map[c] = s2[n2 - 1];
            k++;
        }
    }

    /* "the last specified ARRAY": with two sets that is set2, and the
     * squeeze happens after the translation, so `tr -s a b` on "abb"
     * gives one b and not three. */
    const bool *sqset = (n2 > 0) ? in_set2 : in_set1;

    unsigned char in[8192], out[8192];
    int last = -1;
    for (;;) {
        long n = lp_read(STDIN_FILENO, in, sizeof in);
        if (n <= 0) break;
        int o = 0;
        for (long k = 0; k < n; k++) {
            unsigned char c = in[k];
            if (del && in_set1[c]) continue;
            unsigned char v = (!del && in_set1[c]) ? map[c] : c;
            if (squeeze && sqset[v] && v == last) continue;
            out[o++] = v;
            last = v;
        }
        if (o && lp_write(STDOUT_FILENO, out, (size_t)o) < 0) return 1;
    }
    return 0;
}
