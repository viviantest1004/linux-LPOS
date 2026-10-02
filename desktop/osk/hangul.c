/*
 * hangul.c - the 2-beolsik automaton. See hangul.h for why the state is a
 * list of keystrokes.
 *
 * The rules are the ones libhangul's "2" keyboard and the Windows IME
 * agree on:
 *
 *   consonant after nothing, or after a lone consonant or lone vowel:
 *       starts a new syllable (ㄱ + ㄱ is ㄱㄱ; ㄲ is Shift+ㄱ, not a double tap)
 *   consonant after initial+medial:
 *       becomes the final if it can be one (ㄸ ㅃ ㅉ cannot), else starts anew
 *   consonant after a single final:
 *       joins it when the pair is a compound final (ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ)
 *   vowel after a lone consonant:        becomes the medial
 *   vowel after a single medial:         joins it when the pair is a
 *                                        compound vowel (ㅘ ㅙ ㅚ ㅝ ㅞ ㅟ ㅢ)
 *   vowel after a syllable with a final: the final's last consonant moves
 *                                        into a new syllable (닭 + ㅏ = 달가)
 *   anything else:                       finishes the syllable, starts anew
 *
 * Code points, not indices, are passed around inside, because the
 * compatibility jamo block happens to list the vowels in exactly the
 * medial order of the syllable formula, and the consonants in an order
 * the two small tables below turn into initial and final indices.
 */
#include "hangul.h"

#define SBASE 0xAC00u
#define CBASE 0x3131u   /* ㄱ, first compatibility consonant */
#define CLAST 0x314Eu   /* ㅎ */
#define VBASE 0x314Fu   /* ㅏ, first compatibility vowel */
#define VLAST 0x3163u   /* ㅣ */

/* For each compatibility consonant U+3131..U+314E: its initial index in the
 * syllable formula (-1: cannot start a syllable, the compound finals) and
 * its final index (0: cannot end one - ㄸ ㅃ ㅉ). */
static const signed char cho_of[30] = {
     0,  1, -1,  2, -1, -1,  3,  4,  5, -1, -1, -1, -1, -1, -1, -1,
     6,  7,  8, -1,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
};
static const unsigned char jong_of[30] = {
     1,  2,  3,  4,  5,  6,  7,  0,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17,  0, 18, 19, 20, 21, 22,  0, 23, 24, 25, 26, 27,
};

bool hangul_is_consonant(uint32_t cp)
{
    return cp >= CBASE && cp <= CLAST && cho_of[cp - CBASE] >= 0;
}

bool hangul_is_vowel(uint32_t cp)
{
    /* The fourteen on the keyboard. The compound seven are made here, by
     * combining, and are not accepted as keystrokes. */
    switch (cp) {
    case 0x314F: case 0x3150: case 0x3151: case 0x3152: case 0x3153:
    case 0x3154: case 0x3155: case 0x3156: case 0x3157: case 0x315B:
    case 0x315C: case 0x3160: case 0x3161: case 0x3163:
        return true;
    }
    return false;
}

bool hangul_is_jamo(uint32_t cp)
{
    return hangul_is_consonant(cp) || hangul_is_vowel(cp);
}

static bool is_cons(uint32_t cp) { return cp >= CBASE && cp <= CLAST; }

static uint32_t combine_vowel(uint32_t a, uint32_t b)
{
    if (a == 0x3157) {                     /* ㅗ */
        if (b == 0x314F) return 0x3158;    /* ㅘ */
        if (b == 0x3150) return 0x3159;    /* ㅙ */
        if (b == 0x3163) return 0x315A;    /* ㅚ */
    } else if (a == 0x315C) {              /* ㅜ */
        if (b == 0x3153) return 0x315D;    /* ㅝ */
        if (b == 0x3154) return 0x315E;    /* ㅞ */
        if (b == 0x3163) return 0x315F;    /* ㅟ */
    } else if (a == 0x3161 && b == 0x3163) {
        return 0x3162;                     /* ㅡ + ㅣ = ㅢ */
    }
    return 0;
}

static uint32_t combine_final(uint32_t a, uint32_t b)
{
    switch (a) {
    case 0x3131: return b == 0x3145 ? 0x3133 : 0;            /* ㄱㅅ ㄳ */
    case 0x3134: return b == 0x3148 ? 0x3135 :               /* ㄴㅈ ㄵ */
                        b == 0x314E ? 0x3136 : 0;            /* ㄴㅎ ㄶ */
    case 0x3139:                                             /* ㄹ */
        switch (b) {
        case 0x3131: return 0x313A;                          /* ㄺ */
        case 0x3141: return 0x313B;                          /* ㄻ */
        case 0x3142: return 0x313C;                          /* ㄼ */
        case 0x3145: return 0x313D;                          /* ㄽ */
        case 0x314C: return 0x313E;                          /* ㄾ */
        case 0x314D: return 0x313F;                          /* ㄿ */
        case 0x314E: return 0x3140;                          /* ㅀ */
        }
        return 0;
    case 0x3142: return b == 0x3145 ? 0x3144 : 0;            /* ㅂㅅ ㅄ */
    }
    return 0;
}

/* The syllable the keystroke list spells, taken apart. */
typedef struct {
    uint32_t cho, jung, jong;   /* compatibility code points, 0 = absent */
    int      njung, njong;      /* keystrokes each one took */
} Parts;

static Parts parse(const HangulIC *h)
{
    Parts p = {0, 0, 0, 0, 0};
    int i = 0;
    if (i < h->n && is_cons(h->keys[i]))
        p.cho = h->keys[i++];
    if (i < h->n && !is_cons(h->keys[i])) {
        p.jung = h->keys[i++];
        p.njung = 1;
        if (i < h->n && !is_cons(h->keys[i])) {
            p.jung = combine_vowel(p.jung, h->keys[i++]);
            p.njung = 2;
        }
    }
    if (i < h->n) {
        p.jong = h->keys[i++];
        p.njong = 1;
        if (i < h->n) {
            p.jong = combine_final(p.jong, h->keys[i++]);
            p.njong = 2;
        }
    }
    return p;
}

void hangul_reset(HangulIC *h)
{
    h->n = 0;
}

uint32_t hangul_preedit(const HangulIC *h)
{
    if (h->n == 0)
        return 0;
    Parts p = parse(h);
    if (p.cho && p.jung) {
        uint32_t l = (uint32_t)cho_of[p.cho - CBASE];
        uint32_t v = p.jung - VBASE;
        uint32_t t = p.jong ? jong_of[p.jong - CBASE] : 0;
        return SBASE + (l * 21 + v) * 28 + t;
    }
    return p.cho ? p.cho : p.jung;
}

static void emit(HangulOut *out, uint32_t cp)
{
    if (cp && out->n < (int)(sizeof out->text / sizeof out->text[0]))
        out->text[out->n++] = cp;
}

uint32_t hangul_flush(HangulIC *h)
{
    uint32_t c = hangul_preedit(h);
    h->n = 0;
    return c;
}

/* Finish what is there and start again from one keystroke. */
static void restart(HangulIC *h, uint32_t jamo, HangulOut *out)
{
    emit(out, hangul_flush(h));
    h->keys[0] = jamo;
    h->n = 1;
}

static void push(HangulIC *h, uint32_t jamo)
{
    h->keys[h->n++] = jamo;
}

void hangul_feed(HangulIC *h, uint32_t jamo, HangulOut *out)
{
    if (!hangul_is_jamo(jamo))
        return;
    if (h->n == 0) {
        push(h, jamo);
        return;
    }
    Parts p = parse(h);

    if (hangul_is_consonant(jamo)) {
        if (!p.cho || !p.jung)
            restart(h, jamo, out);          /* lone consonant or lone vowel */
        else if (!p.jong && jong_of[jamo - CBASE])
            push(h, jamo);
        else if (!p.jong)
            restart(h, jamo, out);          /* ㄸ ㅃ ㅉ cannot end one */
        else if (p.njong == 1 && combine_final(p.jong, jamo))
            push(h, jamo);
        else
            restart(h, jamo, out);
        return;
    }

    /* a vowel */
    if (!p.jung) {                          /* a lone initial takes it */
        push(h, jamo);
    } else if (!p.jong) {
        if (p.njung == 1 && combine_vowel(p.jung, jamo))
            push(h, jamo);
        else
            restart(h, jamo, out);
    } else {
        /* Migration. The last keystroke is always a consonant here and is
         * always one that can start a syllable: a compound final was built
         * from two, and only its second one moves. */
        uint32_t moved = h->keys[--h->n];
        emit(out, hangul_flush(h));
        h->keys[0] = moved;
        h->keys[1] = jamo;
        h->n = 2;
    }
}

bool hangul_backspace(HangulIC *h)
{
    if (h->n == 0)
        return false;
    h->n--;
    return true;
}
