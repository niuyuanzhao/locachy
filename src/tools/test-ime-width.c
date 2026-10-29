/*
 * test-ime-width USERDIR: Chinese mode's punctuation (base/ime.c) with the system's
 * Rime data, on a copy of its user directory (the bar has the real one open).
 * Built with -DCONFIG_IME and -lrime.
 *   1. Chinese mode starts at full width: Chinese punctuation; digits and
 *      space still pass through; the label is the full-width one
 *   2. half width (Ctrl+. or Shift+Space): punctuation, digits and space pass
 *      through to the application; the label is the half-width one
 *   3. at half width a comma while composing commits the composition, then
 *      passes through
 *   4. EN mode passes punctuation through and has no width; back in Chinese
 *      mode, full width again
 */
#include <stdio.h>
#include <string.h>

#include "base.h"
#include "ime.h"

static int fail;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fail |= !ok;
}

/* The key's result: the committed text, then "+pass" if the key goes on to
 * the application; "held" if it was taken with nothing committed yet. */
static const char *type(int key)
{
    static char out[64];
    int taken = ime_process_key(key);
    const char *c = ime_get_commit();
    snprintf(out, sizeof out, "%s%s", c ? c : "", taken ? (c ? "" : "held") : "+pass");
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 2 || ime_init(NULL, argv[1]) < 0) {
        fprintf(stderr, "usage: test-ime-width USERDIR (with Rime)\n");
        return 2;
    }
    ime_set_mode(IME_MODE_CN);
    printf("     label: %s\n", ime_mode_label());
    check("Chinese mode starts at full width (中+全)", strcmp(ime_mode_label(), "中+全") == 0);
    check("full width: ',' gives '，'", strcmp(type(','), "，") == 0);
    check("full width: '?' gives '？'", strcmp(type('?'), "？") == 0);
    check("full width: '!' gives '！'", strcmp(type('!'), "！") == 0);
    check("full width: '(' gives '（'", strcmp(type('('), "（") == 0);
    check("full width: '\\' gives '、'", strcmp(type('\\'), "、") == 0);
    check("full width: '1' still passes through", strcmp(type('1'), "+pass") == 0);
    check("full width: ' ' still passes through", strcmp(type(' '), "+pass") == 0);

    const char *msg = ime_toggle_width();
    printf("     notice: %s, label: %s\n", msg ? msg : "(none)", ime_mode_label());
    check("Ctrl+.: half width (中+半)", msg != NULL && strcmp(ime_mode_label(), "中+半") == 0);
    check("half width: ',' passes through", strcmp(type(','), "+pass") == 0);
    check("half width: '?' passes through", strcmp(type('?'), "+pass") == 0);
    check("half width: '1' passes through", strcmp(type('1'), "+pass") == 0);
    check("half width: ' ' passes through", strcmp(type(' '), "+pass") == 0);
    type('n');
    type('i');
    const char *r = type(',');
    printf("     n i , -> %s\n", r);
    check("half width: ',' while composing commits it, then passes through",
          strlen(r) > 5 && strcmp(r + strlen(r) - 5, "+pass") == 0 && r[0] != '+');

    ime_set_mode(IME_MODE_EN);
    check("EN mode: ',' passes through", strcmp(type(','), "+pass") == 0);
    check("EN mode: no width to switch", ime_toggle_width() == NULL);
    ime_set_mode(IME_MODE_RAW);
    ime_set_mode(IME_MODE_CN);
    check("back in Chinese mode: full width again", strcmp(type('?'), "？") == 0 &&
          strcmp(ime_mode_label(), "中+全") == 0);
    ime_done();
    puts(fail ? "SOME FAILED" : "ALL PASSED");
    return fail;
}
