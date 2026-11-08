/*
 * duress_login_check (duress.h): whether a typed password is the user's login
 * password, by /etc/shadow (libxcrypt's crypt_r, as pam_unix checks it). The
 * duress check asks this first, so the real password is never taken for the
 * duress one, whatever refused it before (pam_faillock's lock after wrong
 * tries refuses the right password too). Root only.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <crypt.h>
#include <shadow.h>
#include <stdlib.h>
#include <string.h>

#include "duress.h"

int duress_login_check(const char *user, const char *pw)
{
    struct spwd entry, *found = NULL;
    char buf[4096];
    int result = -1;
    if (!user || !user[0] || !pw) {
        return -1;
    }
    if (getspnam_r(user, &entry, buf, sizeof buf, &found) != 0 || !found || !found->sp_pwdp) {
        explicit_bzero(buf, sizeof buf);
        return -1;
    }
    const char *stored = found->sp_pwdp;
    if (stored[0] == '\0' || stored[0] == '!' || stored[0] == '*') {
        result = 0; /* no password to type: the account cannot be the real one */
    } else {
        struct crypt_data *data = calloc(1, sizeof *data);
        const char *hash = data ? crypt_r(pw, stored, data) : NULL;
        size_t want = strlen(stored);
        int same = hash && hash[0] != '*' && strlen(hash) == want;
        unsigned diff = 0;
        for (size_t i = 0; same && i < want; i++) diff |= (unsigned char)hash[i] ^ (unsigned char)stored[i];
        result = same && diff == 0 ? 1 : 0;
        if (data) {
            explicit_bzero(data, sizeof *data);
            free(data);
        }
    }
    explicit_bzero(buf, sizeof buf);
    return result;
}
