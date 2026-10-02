/* regex.h - a small backtracking regular expression engine.
 *
 * grep has its own matcher, written when the only thing that needed one
 * was grep. sed and awk both need capture groups, alternation and
 * bounded repeats, and three separate half-engines in one system is how
 * you end up with three different ideas of what `a|b` means. So this is
 * the shared one.
 *
 * It compiles to instructions and runs them with backtracking - the
 * approach in Russ Cox's "Regular Expression Matching: the Virtual
 * Machine Approach". Backtracking can be made to take exponential time
 * by a pattern written to do that, so there is a step budget: a search
 * that exceeds it stops and reports no match rather than hanging the
 * board. A wrong answer on a pathological pattern is better than an
 * appliance that has to be power-cycled.
 *
 * Syntax, with ere = false (what sed and grep call basic):
 *
 *   .  any character          *  none or more of the last thing
 *   ^  start                  $  end
 *   [abc] [^abc] [a-z] [[:digit:]]
 *   \(...\)  a group          \|  alternation
 *   \+ \?    one-or-more, optional
 *   \{2,5\}  a counted repeat
 *   \< \>    the edge of a word
 *   \w \W \s \S \d \D
 *
 * With ere = true, ( ) | + ? { } mean those things without the
 * backslash, and a backslash makes them literal. That is the only
 * difference.
 */
#ifndef _LP_REGEX_H
#define _LP_REGEX_H

#include "types.h"

#define RE_MAX_GROUPS 10                     /* \1 .. \9 */
#define RE_MAX_CAPS   (2 * RE_MAX_GROUPS)

typedef struct lpre lpre;

/* Compile. Returns NULL and points *err at a sentence saying what is
 * wrong with the pattern - the message is meant to be printed as-is. */
lpre *re_compile(const char *pattern, bool ere, bool icase, const char **err);
void  re_free(lpre *re);

/* How many \( \) groups the pattern has. */
int   re_ngroups(const lpre *re);

/* Search text for the leftmost match at or after `from`.
 *
 * caps must have room for RE_MAX_CAPS entries. On success caps[0] and
 * caps[1] are the byte offsets of the whole match, caps[2n]/caps[2n+1]
 * of group n; an unmatched group is -1/-1.
 *
 * `notbol` says the text does not start at the beginning of a line, so
 * ^ must not match at `from`. sed needs it when it substitutes
 * repeatedly along one line. */
bool  re_search(lpre *re, const char *text, int from, bool notbol,
                int *caps);

/* Did the last re_search stop because it ran out of steps rather than
 * because there was no match?
 *
 * Without this the two are indistinguishable, and a pattern like
 * \(a*\)* against a long line of a's makes sed quietly not substitute -
 * which reads as "that pattern does not match" and is not true. Check it
 * after a search that found nothing and say so; the answer is about the
 * most recent search only. */
bool  re_gave_up(const lpre *re);

#endif /* _LP_REGEX_H */
