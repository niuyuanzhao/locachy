/* Check optional input-language selection independently of the system locale,
 * and rc.conf's key_ime_width_cn as the direct programs match it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "base.h"
#undef RC_CONF
#define RC_CONF "/nonexistent/rc.conf" /* never found: the width keys are read each time */
#include "../base/ime.c"

static int enabled;
static const char *width_keys_rc; /* key_ime_width_cn; NULL: not in rc.conf */

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    fn("ime_chinese", enabled ? "1" : "0", arg);
    if (width_keys_rc) fn("key_ime_width_cn", width_keys_rc, arg);
    return 0;
}

int main(int argc, char **argv)
{
    enabled = argc > 1 && strcmp(argv[1], "enabled") == 0;
    load_mode_labels();
    assert(ime_mode() == IME_MODE_RAW);
    if (enabled) {
        assert(ime_cycle_mode() == IME_MODE_CN);
        /* Native labels are UTF-8 resources, not the global system UI locale. */
        assert(strstr(ime_mode_desc(), "Pinyin") == NULL);
        printf("Chinese mode: %s\n", ime_mode_desc());
        assert(ime_cycle_mode() == IME_MODE_EN);
    } else {
        assert(ime_cycle_mode() == IME_MODE_EN);
        ime_set_mode(IME_MODE_CN);
        assert(ime_mode() == IME_MODE_EN);
        puts("Chinese mode disabled: skipped during mode cycling.");
    }
    assert(ime_cycle_mode() == IME_MODE_RAW);

    /* No rc.conf at RC_CONF (stat fails), so each call reads the stub again. */
    assert(ime_width_key('.', KEYS_CTRL) && ime_width_key(' ', KEYS_SHIFT));
    assert(!ime_width_key('.', 0) && !ime_width_key(' ', 0) && !ime_width_key('.', KEYS_CTRL | KEYS_SHIFT));
    width_keys_rc = "Alt+w, Ctrl+Shift+Space";
    assert(ime_width_key('w', KEYS_ALT) && ime_width_key('W', KEYS_ALT));
    assert(ime_width_key(' ', KEYS_CTRL | KEYS_SHIFT) && ime_width_key(' ', KEYS_CTRL | KEYS_SHIFT | KEYS_ALTGR));
    assert(!ime_width_key('.', KEYS_CTRL) && !ime_width_key(' ', KEYS_SHIFT));
    puts("PASS key_ime_width_cn: Ctrl+. and Shift+Space unless set; the keys rc.conf names instead");
    ime_done();
    return 0;
}
