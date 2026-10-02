/* clear - wipe the screen.
 *
 * Two escape sequences: put the cursor at the top left, then erase
 * everything. There is no terminfo lookup here because there is one
 * terminal - the kernel's own console - and it understands these.
 */
#include "types.h"
#include "stdio.h"
#include "unistd.h"

int main(void)
{
    fputs("\x1b[H\x1b[2J", STDOUT_FILENO);
    return 0;
}
