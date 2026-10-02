/* whoami - which user this is.
 *
 * Usually root. Not always any more: dropprivs and su both leave a
 * process running as somebody else, and then this is the fastest way to
 * find out which - "am I root here" is worth being able to ask without
 * thinking about it.
 *
 * The name comes from /etc/passwd. A uid with no line there prints as a
 * number, which is the honest answer rather than a guess.
 */
#include "types.h"
#include "stdio.h"
#include "unistd.h"

int main(void)
{
    uid_t uid = (uid_t)lp_getuid();

    lp_user_t u;
    if (lp_user_by_uid(uid, &u))
        printf("%s\n", u.name);
    else
        printf("%d\n", (int)uid);
    return 0;
}
