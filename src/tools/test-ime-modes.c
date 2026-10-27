/* Check optional input-language selection independently of the system locale. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "base.h"
#include "../base/ime.c"

static int enabled;

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    fn("ime_chinese", enabled ? "1" : "0", arg);
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
    ime_done();
    return 0;
}
