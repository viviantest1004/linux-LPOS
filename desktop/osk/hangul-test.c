/*
 * hangul-test.c - checks hangul.c against Unicode, not against itself.
 *
 * `make check` builds and runs this. Nothing here links GTK or Wayland,
 * so it runs anywhere a C compiler does.
 *
 * The expected answers never come from hangul.c. They come from:
 *
 *   - the Unicode syllable formula, S = U+AC00 + (L*21 + V)*28 + T, with
 *     L, V and T looked up in the three jamo orders written out below as
 *     plain Korean text (the order Unicode chapter 3.12 lists them in);
 *   - how each compound jamo is typed, written out as text too (ㅘ is ㅗㅏ,
 *     ㄺ is ㄹㄱ, ...), rather than hangul.c's switch statements;
 *   - a second, offline parser that looks ahead at the whole keystroke
 *     string instead of running a state machine. The two are written
 *     differently on purpose; if they agree on a few hundred thousand
 *     random strings, a mistake would have to be made twice, in two
 *     shapes, to get through.
 *
 * Groups, each reported as pass/fail counts:
 *   1. every one of the 11,172 syllables, typed canonically: the preedit
 *      after every keystroke, the committed result, and backspace taking
 *      it apart one jamo at a time in the reverse order;
 *   2. final-consonant migration: every syllable with a final, followed by
 *      each of the 14 vowel keys (150,822 cases);
 *   3. random keystroke strings, every prefix compared with the offline
 *      parser;
 *   4. hand-picked sequences with literal expected text, including
 *      backspace and non-jamo keys, for the cases people actually hit.
 */
#include "hangul.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tables, as text ────────────────────────────────────────────── */

static const char CHO_S[]  = "ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
static const char JUNG_S[] = "ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ";
static const char JONG_S[] = "ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ";
/* The keys of a 2-beolsik keyboard: 14 + 5 shifted consonants, 10 + 4
 * vowels. */
static const char KEYS_S[] =
    "ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ" "ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅛㅜㅠㅡㅣ";
static const struct { const char *jamo, *typed; } SPLIT[] = {
    {"ㅘ", "ㅗㅏ"}, {"ㅙ", "ㅗㅐ"}, {"ㅚ", "ㅗㅣ"}, {"ㅝ", "ㅜㅓ"},
    {"ㅞ", "ㅜㅔ"}, {"ㅟ", "ㅜㅣ"}, {"ㅢ", "ㅡㅣ"},
    {"ㄳ", "ㄱㅅ"}, {"ㄵ", "ㄴㅈ"}, {"ㄶ", "ㄴㅎ"}, {"ㄺ", "ㄹㄱ"},
    {"ㄻ", "ㄹㅁ"}, {"ㄼ", "ㄹㅂ"}, {"ㄽ", "ㄹㅅ"}, {"ㄾ", "ㄹㅌ"},
    {"ㄿ", "ㄹㅍ"}, {"ㅀ", "ㄹㅎ"}, {"ㅄ", "ㅂㅅ"},
};
#define NSPLIT (int)(sizeof SPLIT / sizeof SPLIT[0])

static uint32_t CHO[19], JUNG[21], JONG[27], KEYS[33];
static uint32_t SPLIT_J[NSPLIT], SPLIT_A[NSPLIT], SPLIT_B[NSPLIT];

static int utf8_decode(const char *s, uint32_t *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max) {
        uint32_t c;
        if (*p < 0x80)      { c = *p++; }
        else if (*p < 0xE0) { c = (uint32_t)(*p++ & 0x1F) << 6;  c |= *p++ & 0x3F; }
        else if (*p < 0xF0) { c = (uint32_t)(*p++ & 0x0F) << 12; c |= (uint32_t)(*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        else                { c = (uint32_t)(*p++ & 0x07) << 18; c |= (uint32_t)(*p++ & 0x3F) << 12;
                              c |= (uint32_t)(*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        out[n++] = c;
    }
    return n;
}

static int utf8_encode(uint32_t c, char *o)
{
    if (c < 0x80)    { o[0] = (char)c; return 1; }
    if (c < 0x800)   { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                       o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static void to_utf8(const uint32_t *s, int n, char *out)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += utf8_encode(s[i], out + k);
    out[k] = 0;
}

static int index_of(const uint32_t *t, int n, uint32_t c)
{
    for (int i = 0; i < n; i++)
        if (t[i] == c)
            return i;
    return -1;
}

static uint32_t syllable(int l, int v, int t)
{
    return 0xAC00u + (uint32_t)((l * 21 + v) * 28 + t);
}

/* How a jamo is typed: itself, or the two keys SPLIT lists. */
static int typed(uint32_t j, uint32_t *out)
{
    for (int i = 0; i < NSPLIT; i++)
        if (SPLIT_J[i] == j) {
            out[0] = SPLIT_A[i];
            out[1] = SPLIT_B[i];
            return 2;
        }
    out[0] = j;
    return 1;
}

static uint32_t joined(uint32_t a, uint32_t b)
{
    for (int i = 0; i < NSPLIT; i++)
        if (SPLIT_A[i] == a && SPLIT_B[i] == b)
            return SPLIT_J[i];
    return 0;
}

static int is_vowel_key(uint32_t c) { return index_of(JUNG, 21, c) >= 0; }
static int is_cons_key(uint32_t c)  { return index_of(CHO, 19, c) >= 0; }

/* ── the offline parser ─────────────────────────────────────────── */

static int reference(const uint32_t *s, int n, uint32_t *out)
{
    int i = 0, k = 0;
    while (i < n) {
        if (is_cons_key(s[i]) && i + 1 < n && is_vowel_key(s[i + 1])) {
            uint32_t l = s[i], v = s[i + 1], t = 0;
            i += 2;
            if (i < n && is_vowel_key(s[i]) && joined(v, s[i]))
                v = joined(v, s[i++]);
            /* A consonant belongs to this syllable only if no vowel
             * follows it - otherwise it is the next one's initial. */
            if (i < n && is_cons_key(s[i]) && index_of(JONG, 27, s[i]) >= 0 &&
                !(i + 1 < n && is_vowel_key(s[i + 1]))) {
                t = s[i++];
                if (i < n && is_cons_key(s[i]) && joined(t, s[i]) &&
                    !(i + 1 < n && is_vowel_key(s[i + 1])))
                    t = joined(t, s[i++]);
            }
            out[k++] = syllable(index_of(CHO, 19, l), index_of(JUNG, 21, v),
                                t ? index_of(JONG, 27, t) + 1 : 0);
        } else if (is_vowel_key(s[i])) {
            uint32_t v = s[i++];
            if (i < n && is_vowel_key(s[i]) && joined(v, s[i]))
                v = joined(v, s[i++]);
            out[k++] = v;
        } else {
            out[k++] = s[i++];
        }
    }
    return k;
}

/* ── driving the automaton like an application would ────────────── */

typedef struct {
    HangulIC ic;
    uint32_t text[256];   /* what the application holds */
    int      n;
} Doc;

static void doc_init(Doc *d) { hangul_reset(&d->ic); d->n = 0; }

static void doc_feed(Doc *d, uint32_t c)
{
    HangulOut o = {{0}, 0};
    hangul_feed(&d->ic, c, &o);
    for (int i = 0; i < o.n; i++)
        d->text[d->n++] = o.text[i];
}

static void doc_key(Doc *d, uint32_t c)
{
    if (c == '<') {                       /* backspace */
        if (!hangul_backspace(&d->ic) && d->n > 0)
            d->n--;
    } else if (hangul_is_jamo(c)) {
        doc_feed(d, c);
    } else {                              /* anything else ends it */
        uint32_t f = hangul_flush(&d->ic);
        if (f)
            d->text[d->n++] = f;
        d->text[d->n++] = c;
    }
}

/* Committed text plus the composing character. */
static int doc_show(const Doc *d, uint32_t *out)
{
    memcpy(out, d->text, (size_t)d->n * sizeof *out);
    int n = d->n;
    uint32_t p = hangul_preedit(&d->ic);
    if (p)
        out[n++] = p;
    return n;
}

static int same(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    return na == nb && memcmp(a, b, (size_t)na * sizeof *a) == 0;
}

/* ── groups ─────────────────────────────────────────────────────── */

static int shown_fail;
static void fail_msg(const char *group, const uint32_t *keys, int nk,
                     const char *what)
{
    if (shown_fail++ > 20)
        return;
    char k[256];
    to_utf8(keys, nk, k);
    printf("  FAIL %s: keys \"%s\": %s\n", group, k, what);
}

static void group_syllables(int *pass, int *fail)
{
    for (int l = 0; l < 19; l++)
    for (int v = 0; v < 21; v++)
    for (int t = 0; t < 28; t++) {
        uint32_t keys[6], expect[6];
        int nk = 0, ok = 1;
        keys[nk] = CHO[l]; expect[nk++] = CHO[l];
        uint32_t vk[2], tk[2];
        int nv = typed(JUNG[v], vk);
        expect[nk] = syllable(l, index_of(JUNG, 21, vk[0]), 0); keys[nk++] = vk[0];
        if (nv == 2) { expect[nk] = syllable(l, v, 0); keys[nk++] = vk[1]; }
        if (t) {
            int nt = typed(JONG[t - 1], tk);
            expect[nk] = syllable(l, v, index_of(JONG, 27, tk[0]) + 1); keys[nk++] = tk[0];
            if (nt == 2) { expect[nk] = syllable(l, v, t); keys[nk++] = tk[1]; }
        }
        HangulIC ic;
        hangul_reset(&ic);
        for (int i = 0; i < nk && ok; i++) {
            HangulOut o = {{0}, 0};
            hangul_feed(&ic, keys[i], &o);
            if (o.n != 0 || hangul_preedit(&ic) != expect[i]) {
                ok = 0;
                fail_msg("syllable", keys, i + 1, "wrong preedit or early commit");
            }
        }
        /* backspace walks back through the same states */
        for (int i = nk - 1; i > 0 && ok; i--) {
            if (!hangul_backspace(&ic) || hangul_preedit(&ic) != expect[i - 1]) {
                ok = 0;
                fail_msg("syllable-bksp", keys, nk, "backspace went elsewhere");
            }
        }
        if (ok && (!hangul_backspace(&ic) || hangul_preedit(&ic) != 0 ||
                   hangul_backspace(&ic))) {
            ok = 0;
            fail_msg("syllable-bksp", keys, nk, "did not empty cleanly");
        }
        /* and flush commits exactly the syllable */
        if (ok) {
            hangul_reset(&ic);
            HangulOut o = {{0}, 0};
            for (int i = 0; i < nk; i++)
                hangul_feed(&ic, keys[i], &o);
            if (o.n != 0 || hangul_flush(&ic) != syllable(l, v, t) ||
                hangul_preedit(&ic) != 0) {
                ok = 0;
                fail_msg("syllable-flush", keys, nk, "flush gave something else");
            }
        }
        ok ? (*pass)++ : (*fail)++;
    }
}

static void group_migration(int *pass, int *fail)
{
    static const char VOWEL_KEYS[] = "ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅛㅜㅠㅡㅣ";
    uint32_t vowels[14];
    utf8_decode(VOWEL_KEYS, vowels, 14);
    for (int l = 0; l < 19; l++)
    for (int v = 0; v < 21; v++)
    for (int t = 1; t < 28; t++)
    for (int w = 0; w < 14; w++) {
        uint32_t keys[8], vk[2], tk[2];
        int nk = 0;
        keys[nk++] = CHO[l];
        int nv = typed(JUNG[v], vk);
        for (int i = 0; i < nv; i++) keys[nk++] = vk[i];
        int nt = typed(JONG[t - 1], tk);
        for (int i = 0; i < nt; i++) keys[nk++] = tk[i];
        keys[nk++] = vowels[w];
        /* The last consonant typed leaves; what stays is the final made of
         * the keystrokes before it. */
        uint32_t moved = tk[nt - 1];
        int kept = nt == 2 ? index_of(JONG, 27, tk[0]) + 1 : 0;
        uint32_t want[2] = {
            syllable(l, v, kept),
            syllable(index_of(CHO, 19, moved), index_of(JUNG, 21, vowels[w]), 0),
        };
        Doc d;
        doc_init(&d);
        for (int i = 0; i < nk; i++)
            doc_key(&d, keys[i]);
        uint32_t got[16];
        int ng = doc_show(&d, got);
        if (same(got, ng, want, 2) && d.n == 1) {
            (*pass)++;
        } else {
            (*fail)++;
            fail_msg("migration", keys, nk, "wrong split");
        }
    }
}

static void group_random(int *pass, int *fail, int count)
{
    unsigned long long seed = 0x9E3779B97F4A7C15ull;
    for (int c = 0; c < count; c++) {
        uint32_t keys[12];
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        int nk = 1 + (int)((seed >> 33) % 12);
        for (int i = 0; i < nk; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            keys[i] = KEYS[(seed >> 33) % 33];
        }
        Doc d;
        doc_init(&d);
        int ok = 1;
        for (int i = 0; i < nk && ok; i++) {
            doc_key(&d, keys[i]);
            uint32_t got[32], want[32];
            int ng = doc_show(&d, got);
            int nw = reference(keys, i + 1, want);
            if (!same(got, ng, want, nw)) {
                ok = 0;
                char g[128], w[128];
                to_utf8(got, ng, g);
                to_utf8(want, nw, w);
                char msg[300];
                snprintf(msg, sizeof msg, "got \"%s\", reference \"%s\"", g, w);
                fail_msg("random", keys, i + 1, msg);
            }
        }
        ok ? (*pass)++ : (*fail)++;
    }
}

static void group_named(int *pass, int *fail)
{
    /* '<' is backspace, ' ' and anything else not a jamo ends the
     * composition and is typed as itself. */
    static const struct { const char *keys, *want; } cases[] = {
        {"ㄷㅏㄹㄱㅏ", "달가"},                 /* the one in the brief */
        {"ㄷㅏㄹㄱ ㄱㅏㅂㅅㅇㅣ", "닭 값이"},
        {"ㄷㅏㄹㄱㅏ ㄱㅏㅂㅅㅇㅣ", "달가 값이"},
        {"ㅎㅏㄴㄱㅡㄹ ㅇㅣㅂㄹㅕㄱ", "한글 입력"},
        {"ㄱㅗㅏㄴ", "관"}, {"ㅇㅜㅔ", "웨"}, {"ㅇㅡㅣ", "의"}, {"ㅇㅗㅐㅇㅛ", "왜요"},
        {"ㅂㅜㅔㄹㄱ", "뷁"}, {"ㄱㅜㅣ", "귀"}, {"ㅎㅗㅣ", "회"}, {"ㅇㅜㅓㄴ", "원"},
        {"ㅇㅏㄴㅎㅇㅡㄴ", "않은"}, {"ㅇㅣㅆㅇㅓ", "있어"}, {"ㅇㅣㅆㅓ", "이써"},
        {"ㅎㅏㄴㅈㅏ", "한자"}, {"ㅎㅏㄴㅈ", "핝"}, {"ㅇㅏㄴㅈㅇㅏ", "앉아"},
        {"ㄱㅏㄹㄱㅅ", "갉ㅅ"}, {"ㄱㅏㄹㅎㅎㅏ", "갏하"}, {"ㄱㅏㄱㄱㅏ", "각가"},
        {"ㄱㅏㄸ", "가ㄸ"}, {"ㄱㅏㄸㅏ", "가따"}, {"ㄱㅏㅃ", "가ㅃ"}, {"ㄱㅏㅉㅏ", "가짜"},
        {"ㄱㅏㄲ", "갂"}, {"ㄱㅏㄲㅏ", "가까"}, {"ㄱㅏㅆ", "갔"}, {"ㄱㅏㅆㅏ", "가싸"},
        {"ㄱㄱ", "ㄱㄱ"}, {"ㄱㅅ", "ㄱㅅ"}, {"ㄱㄴㅏ", "ㄱ나"}, {"ㅏㅏ", "ㅏㅏ"},
        {"ㅗㅏ", "ㅘ"}, {"ㅜㅓㅣ", "ㅝㅣ"}, {"ㅏㄱㅏ", "ㅏ가"}, {"ㅘㄱ", "ㅘㄱ"},
        {"ㄱㅗㅏㅏ", "과ㅏ"}, {"ㄱㅏㅗ", "가ㅗ"}, {"ㅂㅏㄹㅂㅂ", "밟ㅂ"},
        {"ㅋㅋㅋ", "ㅋㅋㅋ"}, {"ㅎㅎ", "ㅎㅎ"}, {"ㅠㅠ", "ㅠㅠ"},
        {"ㅇㅏㄴㄴㅕㅇ", "안녕"}, {"ㅅㅏㄹㅁ", "삶"}, {"ㅅㅏㄹㅁㅇㅡㄴ", "삶은"},
        {"ㅇㅓㅂㅅㄷㅏ", "없다"}, {"ㅇㅓㅂㅅㅇㅓ", "없어"}, {"ㅇㅓㅂㅅㅓ", "업서"},
        /* backspace one jamo at a time */
        {"ㄷㅏㄹㄱ<", "달"}, {"ㄷㅏㄹㄱ<<", "다"}, {"ㄷㅏㄹㄱ<<<", "ㄷ"},
        {"ㄷㅏㄹㄱ<<<<", ""}, {"ㄱㅗㅏ<", "고"}, {"ㄱㅗㅏ<<", "ㄱ"},
        {"ㄷㅏㄹㄱㅏ<", "달ㄱ"}, {"ㄷㅏㄹㄱㅏ<<", "달"}, {"ㄷㅏㄹㄱㅏ<<<", ""},
        {"ㅎㅏㄴ<ㄹ", "할"}, {"ㄱㅏㅂㅅ<ㄹ", "갑ㄹ"}, {"ㄱㅏㅂ<ㄴㅏ", "가나"},
        {"ㅁㅜㅓ<ㅓ", "뭐"}, {"ㄱㅡㄹ<ㅣ", "긔"}, {"ㄱㅏ <", "가"}, {"ㄱㅏ <<", ""},
        /* anything that is not a jamo ends the syllable */
        {"ㄱㅏ.", "가."}, {"ㄱㅏ1ㄴㅏ", "가1나"}, {"ㅎㅏㄴa", "한a"},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint32_t keys[64], want[64], got[64];
        int nk = utf8_decode(cases[c].keys, keys, 64);
        int nw = utf8_decode(cases[c].want, want, 64);
        Doc d;
        doc_init(&d);
        for (int i = 0; i < nk; i++)
            doc_key(&d, keys[i]);
        int ng = doc_show(&d, got);
        if (same(got, ng, want, nw)) {
            (*pass)++;
        } else {
            (*fail)++;
            char g[256];
            to_utf8(got, ng, g);
            printf("  FAIL named: \"%s\" gave \"%s\", want \"%s\"\n",
                   cases[c].keys, g, cases[c].want);
        }
    }
}

int main(void)
{
    utf8_decode(CHO_S, CHO, 19);
    utf8_decode(JUNG_S, JUNG, 21);
    utf8_decode(JONG_S, JONG, 27);
    utf8_decode(KEYS_S, KEYS, 33);
    for (int i = 0; i < NSPLIT; i++) {
        uint32_t two[2];
        utf8_decode(SPLIT[i].jamo, &SPLIT_J[i], 1);
        utf8_decode(SPLIT[i].typed, two, 2);
        SPLIT_A[i] = two[0];
        SPLIT_B[i] = two[1];
    }

    int total_fail = 0, p, f;
    struct { const char *name; void (*fn)(int *, int *); } groups[] = {
        {"every syllable (type, preedit, backspace, flush)", group_syllables},
        {"final-consonant migration, every syllable x 14 vowels", group_migration},
        {"hand-picked sequences", group_named},
    };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        p = f = 0;
        groups[g].fn(&p, &f);
        printf("%-58s pass %6d  fail %d\n", groups[g].name, p, f);
        total_fail += f;
    }
    p = f = 0;
    group_random(&p, &f, 300000);
    printf("%-58s pass %6d  fail %d\n",
           "random strings vs offline parser, every prefix", p, f);
    total_fail += f;

    printf(total_fail ? "FAILED\n" : "all passed\n");
    return total_fail ? 1 : 0;
}
