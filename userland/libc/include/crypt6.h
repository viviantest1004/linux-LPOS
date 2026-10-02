/* crypt6.h - SHA-512 crypt ($6$) and the shadow-file password check
 * shared by passwd, sudo, login, lp-privd and lp-diskd. See crypt6.c. */
#ifndef LP_CRYPT6_H
#define LP_CRYPT6_H

#include "types.h"

#define LP_CRYPT6_PW_MAX 256

/* $6$[rounds=N$]salt$hash into out. */
void lp_crypt6(const char *pw, const char *salt, unsigned rounds,
               char *out, size_t outn);
/* Does pw produce the stored $6$ hash? Constant time in the comparison. */
bool lp_crypt6_verify(const char *pw, const char *stored);
/* A fresh $6$ hash with a random salt. */
bool lp_crypt6_new(const char *pw, char *out, size_t outn);

enum {
    LP_SHADOW_OK = 0,
    LP_SHADOW_WRONG,        /* the password is not this account's       */
    LP_SHADOW_NOUSER,       /* no such account in the file              */
    LP_SHADOW_LOCKED,       /* "!" or "*": no password login at all     */
    LP_SHADOW_EMPTY,        /* empty field: never accepted as a check   */
    LP_SHADOW_UNSUPPORTED,  /* not $6$ (yescrypt?) - reset the password */
    LP_SHADOW_NOFILE,       /* cannot read the shadow file (not root?)  */
};
/* Check pw for user against shadow (NULL = /etc/shadow). */
int lp_shadow_check(const char *shadow, const char *user, const char *pw);

#endif
