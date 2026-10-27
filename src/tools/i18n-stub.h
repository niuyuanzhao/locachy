/* For a test that includes a program and links some of base but not
 * base/i18n.c: the interface in English, as written.
 * test-i18n tests the translation itself. */
#ifndef LOCACHY_I18N_STUB_H
#define LOCACHY_I18N_STUB_H

const char *base_tr(const char *msgid) { return msgid; }
const char *base_trc(const char *context, const char *msgid) { (void)context; return msgid; }
const char *base_trn(const char *one, const char *many, unsigned long n) { return n == 1 ? one : many; }
const char *base_language(void) { return ""; }
void base_language_reload(void) {}

#endif
