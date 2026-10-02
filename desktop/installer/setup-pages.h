/*
 * setup-pages.h - the two pages the installer and the first-boot setup
 * both ask: who uses this computer (account), and where it is and what
 * it is called (region).
 *
 * The installer asks them because the recovery password has to exist the
 * moment the disk is written (COMMON.md, Administrator rights: the hash
 * goes onto LP-RECOVERY with the copy). The first-boot setup asks them
 * only when the installer did not - a scripted `lp-install` without
 * --user, or an image written to the disk by hand. Same fields, same
 * rules, same words, so they live here once.
 *
 * Each builder makes a full SuPage with a Back and a Next button; the
 * caller connects the two buttons and reads the answers with the
 * getters. Next is insensitive until every field is valid, and the one
 * line under the fields says what is still wrong - on a touch screen a
 * greyed button with no reason is a dead end.
 */
#ifndef LP_SETUP_PAGES_H
#define LP_SETUP_PAGES_H

#include "setup-ui.h"

G_BEGIN_DECLS

typedef struct {
    SuPage     page;
    GtkWidget *fullname, *login, *pw1, *pw2;
    GtkWidget *hint;
    GtkWidget *back, *next;
    gboolean   login_edited;     /* stop deriving it from the full name */
    gboolean   updating;
} SuAccount;

/* recovery_note: say "Your password also unlocks Recovery." (installer). */
void        su_account_build(SuAccount *a, gboolean recovery_note);
const char *su_account_login(SuAccount *a);
const char *su_account_fullname(SuAccount *a);
const char *su_account_password(SuAccount *a);

typedef struct {
    SuPage         page;
    GtkWidget     *tz, *host, *hint;
    GtkWidget     *back, *next;
    GtkStringList *zones;
    gboolean       tz_touched, host_edited, updating;
} SuRegion;

void        su_region_build(SuRegion *r);
const char *su_region_timezone(SuRegion *r);
const char *su_region_hostname(SuRegion *r);
/* The account's login name changed: suggest "<login>-<model>". */
void        su_region_suggest_host(SuRegion *r, const char *login);
/* The language changed: Korean moves an untouched zone to Asia/Seoul. */
void        su_region_language_changed(SuRegion *r);

G_END_DECLS

#endif
