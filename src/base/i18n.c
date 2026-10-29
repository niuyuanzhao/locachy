/* The interface's language: GNU gettext over the generated locale of
 * rc.conf's language (base.h). glibc ignores LANGUAGE in
 * the C and C.UTF-8 locales, so the language is the LC_MESSAGES locale
 * itself; make.sh generates it for every catalog in po/LINGUAS. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

static int ready;
static char language[32];

static void on_language(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (strcmp(name, "language") == 0) {
        snprintf(language, sizeof language, "%s", value);
    }
}

static int english(const char *l)
{
    return !l[0] || strncmp(l, "en", 2) == 0 || strcmp(l, "C") == 0 || strncmp(l, "C.", 2) == 0 ||
           strcmp(l, "POSIX") == 0;
}

void base_language_reload(void)
{
    if (!ready) {
        const char *dir = getenv("LOCACHY_LOCALE_DIR");
        bindtextdomain(I18N_DOMAIN, dir && dir[0] ? dir : LOCALE_DIR);
        bind_textdomain_codeset(I18N_DOMAIN, "UTF-8");
        /* Other libraries' catalogs (PAM's prompts and messages) come in
         * LC_CTYPE's character set: UTF-8, as everything is drawn here; in
         * the C locale's ASCII every Chinese character is a question mark. */
        if (!setlocale(LC_CTYPE, "C.UTF-8")) {
            say("i18n: no C.UTF-8 locale; other libraries' translations may lose characters");
        }
        ready = 1;
    }
    language[0] = '\0';
    const char *env = getenv("LOCACHY_LANG");
    if (env && env[0]) {
        snprintf(language, sizeof language, "%s", env);
    } else {
        rc_read(on_language, NULL);
    }
    if (english(language)) {
        language[0] = '\0';
        setlocale(LC_MESSAGES, "C.UTF-8");
    } else if (!setlocale(LC_MESSAGES, language)) {
        say("i18n: locale %s is not generated; the interface stays English", language);
        language[0] = '\0';
        setlocale(LC_MESSAGES, "C.UTF-8");
    }
}

const char *base_language(void)
{
    if (!ready) base_language_reload();
    return language;
}

const char *base_tr(const char *msgid)
{
    if (!msgid || !msgid[0]) return msgid; /* "" would be the catalog's header */
    if (!ready) base_language_reload();
    return dgettext(I18N_DOMAIN, msgid);
}

/* pgettext: the catalog's key is "CONTEXT\004MSGID". */
const char *base_trc(const char *context, const char *msgid)
{
    char key[512];
    if (!msgid || !msgid[0] || snprintf(key, sizeof key, "%s\004%s", context, msgid) >= (int)sizeof key) {
        return msgid;
    }
    if (!ready) base_language_reload();
    const char *t = dgettext(I18N_DOMAIN, key);
    return t == key ? msgid : t;
}

const char *base_trn(const char *one, const char *many, unsigned long n)
{
    if (!ready) base_language_reload();
    return dngettext(I18N_DOMAIN, one, many, n);
}
