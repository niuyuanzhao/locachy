/*
 * test-i18n LOCALEDIR: base/i18n.c over the catalog test-i18n.sh made in
 * LOCALEDIR: _, C_ and P_ translate; what the catalog has
 * not, and "", stay as they are; the language changes while running;
 * a language without its generated locale stays English; ui_cells and
 * ui_pad count a CJK character as two cells.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

/* ui.c's screen and keyboard, not drawn here. */
int screen_char_cells(uint32_t cp) { return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xFF01 && cp <= 0xFF60) ? 2 : 1; }
int screen_cols(void) { return 80; }
int screen_rows(void) { return 25; }
void screen_fill(int c, int r, int w, int h, uint32_t rgb) { (void)c; (void)r; (void)w; (void)h; (void)rgb; }
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg) { (void)c; (void)r; (void)s; (void)fg; (void)bg; }
void screen_flush(void) {}
int key_poll_timeout(int fd, int timeout) { (void)fd; (void)timeout; return 27; }
const struct pointer_event *pointer_last(void) { return NULL; }

static void language(const char *l)
{
    setenv("LOCACHY_LANG", l, 1);
    base_language_reload();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    setenv("LOCACHY_LOCALE_DIR", argv[1], 1);
    setenv("LOCACHY_LANG", "zh_CN.UTF-8", 1);

    printf("== translated where marked, the rest as it is\n");
    assert(strcmp(_("Save"), "\xe4\xbf\x9d\xe5\xad\x98") == 0);
    assert(strcmp(_("Not in the catalog"), "Not in the catalog") == 0);
    assert(strcmp(_(""), "") == 0); /* not the catalog's header */
    assert(strcmp(base_language(), "zh_CN.UTF-8") == 0);

    printf("== a context tells the same English apart\n");
    assert(strcmp(C_("verb", "Open"), "\xe6\x89\x93\xe5\xbc\x80") == 0);
    assert(strcmp(C_("state", "Open"), "\xe5\xb7\xb2\xe6\x89\x93\xe5\xbc\x80") == 0);
    assert(strcmp(C_("elsewhere", "Open"), "Open") == 0);

    printf("== plurals by the catalog's rule\n");
    assert(strcmp(P_("%d item", "%d items", 1), "%d \xe9\xa1\xb9") == 0);
    assert(strcmp(P_("%d item", "%d items", 5), "%d \xe9\xa1\xb9") == 0);

    printf("== the language changes while running\n");
    language("en_US.UTF-8");
    assert(strcmp(_("Save"), "Save") == 0 && base_language()[0] == '\0');
    assert(strcmp(P_("%d item", "%d items", 5), "%d items") == 0);
    language("zh_CN.UTF-8");
    assert(strcmp(_("Save"), "\xe4\xbf\x9d\xe5\xad\x98") == 0);

    printf("== a language whose locale is not generated stays English\n");
    language("fr_FR.UTF-8");
    assert(strcmp(_("Save"), "Save") == 0 && base_language()[0] == '\0');

    printf("== cells, not bytes\n");
    char b[32];
    assert(ui_cells("\xe4\xbf\x9d\xe5\xad\x98") == 4 && ui_cells("Save") == 4 && ui_cells("\xc3\xa9t\xc3\xa9") == 3);
    assert(strcmp(ui_pad(b, sizeof b, "\xe4\xbf\x9d\xe5\xad\x98", 6), "\xe4\xbf\x9d\xe5\xad\x98  ") == 0);
    assert(strcmp(ui_pad(b, sizeof b, "longer", 3), "longer") == 0);
    assert(strcmp(ui_pad(b, 6, "abc", 10), "abc  ") == 0); /* cut to the buffer, terminated */

    printf("ALL PASSED\n");
    return 0;
}
