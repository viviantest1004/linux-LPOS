/* reset - put the terminal back together.
 *
 * A program that dies while it owns the terminal leaves it however it
 * was: no echo, no line editing, newlines that drop a row without
 * returning to the left. Typing then does nothing visible, and the way
 * out is to type a command you cannot see.
 *
 * The shell does this by itself before every prompt, so this is for the
 * case where it is the shell that is confused, or where the terminal is
 * being shared with something else.
 */
#include "types.h"
#include "stdio.h"
#include "unistd.h"

int main(void)
{
    lp_term_sane(STDIN_FILENO);

    /* Leave the screen in a known state too: default colours, cursor
     * visible, at the top left, nothing left over. */
    fputs("\x1b[0m\x1b[?25h\x1b[H\x1b[2J", STDOUT_FILENO);
    return 0;
}
