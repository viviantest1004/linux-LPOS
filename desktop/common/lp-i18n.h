/*
 * lp-i18n.h - two languages, decided once.
 *
 * The machine's default language is English and Korean is the other one
 * it speaks. These three applications are written here, so their strings
 * have to exist twice, and this is where that is arranged.
 *
 * ── Why not gettext ──
 *
 * gettext is the usual answer and it is the wrong size for this. It
 * wants a .pot extracted from the source, a .po per language, msgfmt in
 * the build, and a /usr/share/locale tree in the image - all so that
 * three programs can say two hundred short strings in two languages.
 * Worse, the two halves live in different files, and the moment an
 * English string is edited without the .po being regenerated the Korean
 * one silently keeps saying the old thing.
 *
 * Here both sit on the same line:
 *
 *     gtk_window_set_title(w, T("Settings", "설정"));
 *
 * They cannot drift apart, because changing one and not the other is
 * visible in the diff. The cost is that a translator has to edit C, and
 * that a third language would mean touching every call - at which point
 * gettext becomes the right answer and this file goes away.
 *
 * ── How the language is chosen ──
 *
 * LC_ALL, then LC_MESSAGES, then LANG, which is the order glibc itself
 * resolves them in. Anything beginning with "ko" is Korean; everything
 * else, including an unset environment, is English. English is the
 * fallback rather than Korean because a machine whose locale did not
 * come through should say something the largest number of people can
 * read.
 */

#ifndef LP_I18N_H
#define LP_I18N_H

#include <glib.h>
#include <string.h>

static inline gboolean lp_korean(void)
{
    /* Read once. getenv on every label would be harmless but this is
     * called from inside list-building loops. */
    static int cached = -1;
    if (cached < 0) {
        const char *v = g_getenv("LC_ALL");
        if (!v || !*v) v = g_getenv("LC_MESSAGES");
        if (!v || !*v) v = g_getenv("LANG");
        cached = (v && (g_str_has_prefix(v, "ko_") ||
                        g_strcmp0(v, "ko") == 0)) ? 1 : 0;
    }
    return cached == 1;
}

/* T is for a string shown to a person. Both arguments are literals, so
 * the one not chosen costs nothing but the bytes it occupies. */
#define T(en, ko) (lp_korean() ? (ko) : (en))

#endif /* LP_I18N_H */
