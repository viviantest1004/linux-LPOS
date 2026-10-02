/* echo - print the arguments.
 *
 *   echo [-n] [-e] [text]...
 *
 *   -n  no newline at the end
 *   -e  interpret \n \t \\ and the rest
 *
 * The shell has echo as a builtin, and that is the one that runs when
 * you type it. This file exists for everything that does NOT go through
 * the shell:
 *
 *   printf 'a\nb\n' | xargs echo        xargs execs it directly
 *   find . -name '*.c' -exec echo {} +
 *
 * Those call execve on the name, and a builtin has no file behind it,
 * so they answered "not found" for the single most obvious command on
 * the machine. One tiny binary fixes all of them, and the builtin still
 * wins when a person types it - which is what every other system does
 * too, and why `which echo` and `echo` can disagree everywhere.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void put_escaped(const char *s)
{
    while (*s) {
        if (*s != '\\') { putchar(*s++); continue; }
        s++;
        switch (*s) {
        case 'n':  putchar('\n'); s++; break;
        case 't':  putchar('\t'); s++; break;
        case 'r':  putchar('\r'); s++; break;
        case 'a':  putchar('\a'); s++; break;
        case 'b':  putchar('\b'); s++; break;
        case 'f':  putchar('\f'); s++; break;
        case 'v':  putchar('\v'); s++; break;
        case '\\': putchar('\\'); s++; break;
        case '0': {
            s++;
            int v = 0, k = 0;
            while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s++ - '0'); k++; }
            putchar((char)v);
            break;
        }
        case '\0': putchar('\\'); break;
        default:   putchar('\\'); putchar(*s++); break;
        }
    }
}

int main(int argc, char **argv)
{
    bool newline = true, escapes = false;
    int  i = 1;

    /* Options only while they look like options, and only before the
     * first word. `echo -n` prints nothing; `echo hello -n` prints
     * "hello -n", which is what everybody expects and what a script
     * that echoes a filename beginning with a dash depends on. */
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0)      newline = false;
        else if (strcmp(argv[i], "-e") == 0) escapes = true;
        else if (strcmp(argv[i], "-E") == 0) escapes = false;
        else break;
    }

    for (int k = i; k < argc; k++) {
        if (k > i) putchar(' ');
        if (escapes) put_escaped(argv[k]);
        else         fputs(argv[k], STDOUT_FILENO);
    }
    if (newline) putchar('\n');
    return 0;
}
