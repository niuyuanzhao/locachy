/*
 * pam_locachy_duress.so (/usr/lib/security): the duress password (duress.h)
 * where sudo asks for the password. settings puts it before sudo's first
 * auth line (after pam_locachy_any.so's, when that is there) while rc.conf
 * has duress_enabled="1":
 *   auth  optional  pam_locachy_duress.so
 * It has to come first: Arch's system-auth ends the stack on a wrong
 * password (pam_faillock authfail, [default=die]), so a line after it never
 * sees the duress password. Here it takes the password with pam_get_authtok:
 * the one pam_locachy_any.so read, or it asks. Either way it is left in
 * PAM_AUTHTOK, which pam_unix (try_first_pass) checks without asking again.
 * The login password is checked first (duress_login_check) and always
 * wins: this module never decides anything itself (PAM_IGNORE), it only
 * wipes when the typed text is the duress password and not the login one.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include "duress.h"

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)flags;
    (void)argc;
    (void)argv;
    const char *user = NULL;
    const char *tok = NULL;
    if (!duress_configured() || pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user ||
        pam_get_authtok(pamh, PAM_AUTHTOK, &tok, NULL) != PAM_SUCCESS || !tok) {
        return PAM_IGNORE;
    }
    if (duress_login_check(user, tok) == 0 && duress_matches(tok)) {
        duress_wipe();
    }
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_IGNORE;
}
