/*
 * setup-ui.h - what the installer and the first-boot setup share.
 *
 * Both are full-screen, touch-first, two-language windows that walk a
 * person through a few pages and then do something irreversible. They
 * look the same on purpose - the first-boot setup is the second half of
 * the installation as far as the person is concerned - so the look, the
 * page frame and the language switch live here, once.
 *
 * ── Why not lp-i18n.h's T() ──
 *
 * T() decides the language once, from the environment, and caches it.
 * That is right for an application started inside a session whose
 * language is already settled. Here the language is the first question
 * on the first page, and every page after it - and the page it is asked
 * on - has to change the moment the person taps 한국어. So every label
 * made through su_label()/su_button() keeps both strings, and
 * su_set_korean() rewrites all of them. The pairing on one line is the
 * same as T()'s: English first, Korean second, they cannot drift apart.
 */
#ifndef LP_SETUP_UI_H
#define LP_SETUP_UI_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

extern gboolean su_korean;

/* For strings built at run time (printf formats). */
#define TR(en, ko) (su_korean ? (ko) : (en))

/* Stylesheets: motion.css, then ours, both above the user's theme. */
void       su_load_css(void);

/* A label or button that follows the language. css may be NULL. */
GtkWidget *su_label(const char *en, const char *ko, const char *css);
GtkWidget *su_button(const char *en, const char *ko, const char *css);
/* Give a registered widget new text in both languages. */
void       su_retext(GtkWidget *w, const char *en, const char *ko);
/* Switch every registered widget, and gtk_widget_set_direction stays LTR. */
void       su_set_korean(gboolean ko);

/* A page: a title, a line under it, a body box to fill, and a row of
 * buttons at the bottom (left for "Back", right for the way forward). */
typedef struct {
    GtkWidget *root;        /* add this to the stack                */
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *body;        /* vertical box, fill it                */
    GtkWidget *left;        /* horizontal box for the back button   */
    GtkWidget *right;       /* horizontal box for the forward ones  */
} SuPage;

void       su_page(SuPage *p, const char *en_title, const char *ko_title,
                   const char *en_sub, const char *ko_sub);

/* A large tappable choice: a toggle button with a title and a detail
 * line. Group them with gtk_toggle_button_set_group(). */
GtkWidget *su_choice(const char *en, const char *ko,
                     const char *en_detail, const char *ko_detail,
                     const char *icon);

/* The page stack, with the design system's page transition (260 ms
 * slide, or a 100 ms crossfade when motion is reduced). */
GtkWidget *su_stack(void);
/* Tapping this text field brings up the on-screen keyboard; focus leaving
 * every such field puts it away. For the setup windows, where the input
 * method never announces a focused field (see setup-ui.c). */
void su_osk_attach(GtkWidget *field);
/* The card that moves up while the keyboard is showing. */
void su_osk_card(GtkWidget *card);
GtkWidget *su_card_holder(GtkWidget *card);
/* Go to a page, sliding the right way: forward left, back right. */
void       su_go(GtkWidget *stack, const char *name, gboolean forward);

/* A progress bar that eases towards each new value on a spring instead
 * of jumping, so a copy reported in 1% steps reads as one motion. */
typedef struct _SuProgress SuProgress;
SuProgress *su_progress_new(void);
GtkWidget  *su_progress_widget(SuProgress *p);
void        su_progress_set(SuProgress *p, double fraction);

/* Run argv, collect stdout; NULL on failure. */
char      *su_run(const char *const *argv, int *status);

/* write + fsync + rename + fsync(dir). */
gboolean   su_write_atomic(const char *path, const char *data, int mode,
                           GError **err);

G_END_DECLS

#endif
