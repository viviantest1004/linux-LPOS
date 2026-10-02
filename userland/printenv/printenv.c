/* printenv - one variable, or all of them.
 *
 *   printenv           every NAME=value
 *   printenv PATH      just the value, no name
 *
 * Exits 1 if a name asked for is not set, which is what a script tests.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "null", 0, '0' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    char eol = '\n';
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "0", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case '0': eol = '\0'; break;
        case 'H':
            printf("Usage: printenv [OPTION]... [VARIABLE]...\n"
                   "Print the values of the specified environment VARIABLE(s).\n"
                   "If no VARIABLE is specified, print name=value pairs for them all.\n\n"
                   "  -0, --null     end each output line with NUL, not newline\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("printenv", &g); return 2;
        }
    }

    if (g.ind >= argc) {
        for (char **e = environ; e && *e; e++)
            printf("%s%c", *e, eol);
        return 0;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (!v) { rc = 1; continue; }
        printf("%s%c", v, eol);
    }
    return rc;
}
