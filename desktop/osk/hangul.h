/*
 * hangul.h - the 2-beolsik (두벌식) composition automaton.
 *
 * Both halves of lp-osk type Korean through this: the on-screen keys and,
 * while a text field is focused, the laptop's own keyboard. It is plain C
 * with no GTK or Wayland in it so that hangul-test.c can drive it through
 * every syllable Unicode defines without a display.
 *
 * The state is the list of jamo keystrokes that make up the syllable still
 * being composed, not a (initial, medial, final) triple. That one choice
 * is what makes backspace take off exactly one jamo at a time - 닭 goes
 * back to 달, not to 다 - and what makes final-consonant migration (닭 + ㅏ
 * = 달가) a matter of moving the last keystroke into the next syllable.
 * The triple is recomputed from the list whenever it is needed; the list
 * is at most five long, so that costs nothing.
 *
 * Input and output are Unicode code points. A jamo is fed as its
 * compatibility code point (U+3131..U+3163), which is what the keys are
 * labelled with; a composed syllable comes out from U+AC00..U+D7A3; a lone
 * consonant or vowel comes out as the compatibility jamo, which is what
 * every Korean IME shows for one.
 */
#ifndef LP_HANGUL_H
#define LP_HANGUL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t keys[6];   /* keystrokes of the syllable being composed */
    int      n;
} HangulIC;

/* Characters that leave the composition during one call. Two at most: a
 * migration commits the old syllable, nothing else ever commits more. */
typedef struct {
    uint32_t text[4];
    int      n;
} HangulOut;

void     hangul_reset(HangulIC *h);
bool     hangul_is_jamo(uint32_t cp);      /* one the automaton accepts */
bool     hangul_is_consonant(uint32_t cp);
bool     hangul_is_vowel(uint32_t cp);

/* Feed one jamo; whatever it finishes is appended to out. */
void     hangul_feed(HangulIC *h, uint32_t jamo, HangulOut *out);

/* Take the last jamo off the composition. False when nothing was being
 * composed, which is the caller's cue to send a real BackSpace. */
bool     hangul_backspace(HangulIC *h);

/* The character being composed, 0 when there is none. */
uint32_t hangul_preedit(const HangulIC *h);

/* End the composition and return what it leaves behind (0 if nothing). */
uint32_t hangul_flush(HangulIC *h);

#endif
