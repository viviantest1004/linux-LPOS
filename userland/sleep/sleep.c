/* sleep - wait.
 *
 *   sleep <seconds>
 *   sleep 0.5           fractions are allowed
 *
 * Every system has this and ours did not, which makes a boot script
 * unable to wait for anything - there is no way to say "give the
 * network a moment" without it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: sleep <seconds>\n");
        return 2;
    }

    /* Parse seconds with up to three decimals, without floating point -
     * our printf has none and this does not need it. */
    const char *p = argv[1];
    long secs = 0;
    long ms;

    if (*p < '0' || *p > '9') {
        dprintf(STDERR_FILENO, "sleep: not a number: %s\n", argv[1]);
        return 2;
    }
    /* Count the seconds in ordinary base ten and convert once at the
     * end. Scaling inside the loop multiplies every earlier digit by a
     * thousand a second time - "sleep 33" then waits 3003 seconds, and
     * since one to nine come out right nothing shorter ever shows it. */
    while (*p >= '0' && *p <= '9') {
        if (secs > 100000000L) {        /* longer than three years: say so */
            dprintf(STDERR_FILENO, "sleep: %s is too long\n", argv[1]);
            return 2;
        }
        secs = secs * 10 + (*p++ - '0');
    }
    ms = secs * 1000;

    if (*p == '.') {
        p++;
        long scale = 100;
        while (*p >= '0' && *p <= '9' && scale > 0) {
            ms += (*p++ - '0') * scale;
            scale /= 10;
        }
        while (*p >= '0' && *p <= '9')      /* extra digits: ignore */
            p++;
    }

    if (*p != '\0') {
        dprintf(STDERR_FILENO, "sleep: not a number: %s\n", argv[1]);
        return 2;
    }

    lp_sleep_ms(ms);
    return 0;
}
