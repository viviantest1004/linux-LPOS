/* printf - print with a format, from a script.
 *
 *   printf 'FORMAT' [argument...]
 *
 * `echo` cannot produce a line without a newline on the end, cannot pad
 * a column, and cannot write a tab portably. Every script that formats a
 * table needs this.
 *
 *   printf '%-12s %6d\n' name 42
 *   printf '%s\t%s\n' a b
 *   printf 'no newline'
 *
 * Conversions: %s %d %i %u %x %X %o %c %% , with a width, a - to left
 * justify, and a 0 to pad with zeros. Escapes: \n \t \r \\ \a \b \f \v
 * \0nnn and \xNN.
 *
 * The format is reused while arguments remain, exactly as printf(1) has
 * always done - which is what makes `printf '%s\n' *.txt` print one name
 * per line. Integers only: this libc has no floating point, and a %f
 * that printed a wrong number would be worse than one that says so.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static void out(const char *s, size_t n) { lp_write(STDOUT_FILENO, s, n); }
static void outc(char c) { out(&c, 1); }

/* Escapes in text - both in the format and inside a %b-style argument. */
static void put_escaped(const char *s)
{
    while (*s) {
        if (*s != '\\') { outc(*s++); continue; }
        s++;
        switch (*s) {
        case 'n':  outc('\n'); s++; break;
        case 't':  outc('\t'); s++; break;
        case 'r':  outc('\r'); s++; break;
        case 'a':  outc('\a'); s++; break;
        case 'b':  outc('\b'); s++; break;
        case 'f':  outc('\f'); s++; break;
        case 'v':  outc('\v'); s++; break;
        case '\\': outc('\\'); s++; break;
        case 'x': {
            s++;
            int v = 0, k = 0;
            while (k < 2 && ((*s >= '0' && *s <= '9') ||
                             (*s >= 'a' && *s <= 'f') ||
                             (*s >= 'A' && *s <= 'F'))) {
                int d = (*s <= '9') ? *s - '0'
                      : ((*s | 32) - 'a' + 10);
                v = v * 16 + d; s++; k++;
            }
            outc((char)v);
            break;
        }
        case '0': {
            s++;
            int v = 0, k = 0;
            while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s++ - '0'); k++; }
            outc((char)v);
            break;
        }
        case '\0': outc('\\'); break;
        default:   outc('\\'); outc(*s++); break;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: printf FORMAT [argument...]\n");
        printf("  %%s %%d %%u %%x %%o %%c %%%%, with width and - and 0\n");
        printf("  \\n \\t \\r \\\\ \\xNN \\0nnn\n");
        printf("  whole numbers only - there is no floating point here\n");
        return argc < 2 ? 2 : 0;
    }

    const char *fmt = argv[1];
    int argi = 2;

    do {
        int used_this_pass = 0;
        const char *f = fmt;

        while (*f) {
            if (*f != '%') {
                if (*f == '\\') {
                    /* hand the escape run to put_escaped one item at a
                     * time so a % right after it is still seen here */
                    char esc[8];
                    int n = 0;
                    esc[n++] = *f++;
                    if (*f) esc[n++] = *f++;
                    while (*f && n < (int)sizeof esc - 1 &&
                           ((esc[1] == 'x' && ((*f >= '0' && *f <= '9') ||
                                               ((*f | 32) >= 'a' && (*f | 32) <= 'f'))) ||
                            (esc[1] == '0' && *f >= '0' && *f <= '7')))
                        esc[n++] = *f++;
                    esc[n] = '\0';
                    put_escaped(esc);
                } else outc(*f++);
                continue;
            }

            f++;
            if (*f == '%') { outc('%'); f++; continue; }

            /* flags, then width */
            char spec[32];
            int  s = 0;
            spec[s++] = '%';
            while (*f == '-' || *f == '0' || *f == '+' || *f == ' ')
                if (s < (int)sizeof spec - 8) spec[s++] = *f++; else f++;
            while (*f >= '0' && *f <= '9')
                if (s < (int)sizeof spec - 8) spec[s++] = *f++; else f++;
            if (*f == '.') {
                if (s < (int)sizeof spec - 8) spec[s++] = *f++; else f++;
                while (*f >= '0' && *f <= '9')
                    if (s < (int)sizeof spec - 8) spec[s++] = *f++; else f++;
            }

            char conv = *f ? *f++ : 's';
            const char *arg = (argi < argc) ? argv[argi++] : "";
            used_this_pass++;

            char buf[1024];
            switch (conv) {
            case 's':
                spec[s++] = 's'; spec[s] = '\0';
                snprintf(buf, sizeof buf, spec, arg);
                out(buf, strlen(buf));
                break;
            case 'b':
                /* like %s, but escapes in the argument are expanded */
                put_escaped(arg);
                break;
            case 'c':
                spec[s++] = 'c'; spec[s] = '\0';
                snprintf(buf, sizeof buf, spec, arg[0]);
                out(buf, strlen(buf));
                break;
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                /* "ll", not "l": the value below is s64, and a long is
                 * four bytes on a 32-bit machine - so %ld there would
                 * print half of it and read the wrong stack slot. */
                spec[s++] = 'l';
                spec[s++] = 'l';
                spec[s++] = (conv == 'i') ? 'd' : conv;
                spec[s] = '\0';
                char *end;
                /* Base 0, not 10: GNU takes the C prefixes here, so
                 * `printf '%d' 0x2a` is 42 and not 0. It matters more
                 * than it looks - reading a MAC address or a mode out
                 * of /sys and turning it into a number goes through
                 * exactly this, and the wrong answer is a plausible
                 * one. strtoll, so a value past two billion survives on
                 * a 32-bit machine. */
                /* Base 0 whatever the conversion is. Reading the
                 * argument as hex for %x looked reasonable and meant
                 * `printf '%x' 255` printed 255: parsed as 0x255, then
                 * printed back as hex. The conversion says how to write
                 * the number, not how to read it. */
                s64 v = strtoll(arg, &end, 0);
                if (end == arg && *arg) {
                    dprintf(STDERR_FILENO,
                            "printf: \"%s\" is not a number\n", arg);
                    return 1;
                }
                snprintf(buf, sizeof buf, spec, (long long)v);
                out(buf, strlen(buf));
                break;
            }
            case 'f': case 'e': case 'g':
                dprintf(STDERR_FILENO,
                        "printf: %%%c needs floating point, which this "
                        "system does not have.\n"
                        "printf:   `calc` does integer arithmetic, or use "
                        "python for real numbers.\n", conv);
                return 1;
            default:
                dprintf(STDERR_FILENO,
                        "printf: %%%c is not a conversion I know\n", conv);
                return 1;
            }
        }

        /* Repeat the format while arguments are left - but only if this
         * pass actually consumed one, or a format with no conversions
         * would loop until the board was reset. */
        if (used_this_pass == 0) break;
    } while (argi < argc);

    return 0;
}
