/* The input method as term and locachy-bar have it, linked as they are
 * (build-check.sh's ime-libs):
 * - Rime beside libinput: Rime's Lua plugin and libinput link different Lua
 *   versions with the same function names, and Rime must get its own, or a
 *   Lua panic aborts the program as the Chinese schema loads.
 * - Chinese mode's punctuation: full width at first, for the keys of
 *   default.custom.yaml's table only, each committed at once; the others,
 *   and every key at half width, go on as typed. Right after a digit, "."
 *   and ":" stay as typed (3.14, 12:30: Rime's digit separators, which must
 *   see the digit), the key again giving the full-width stop or colon.
 * - English mode (easy_en): every key typed exactly; a completion only by Tab.
 * test-ime RIME_USER_DIR [DEVICE]: RIME_USER_DIR a copy of
 * /var/lib/locachy/rime deployed with the candidate's default.custom.yaml
 * (Rime writes to it); DEVICE an input device libinput opens (read only, not
 * grabbed) before Rime starts. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "base.h"
#include "ime.h"

static int open_restricted(const char *path, int flags, void *data)
{
    (void)data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *data)
{
    (void)data;
    close(fd);
}

static const struct libinput_interface iface = { open_restricted, close_restricted };

/* Rime's threads would not let the program end normally from here. */
static void fail(void)
{
    fflush(NULL);
    _exit(1);
}

/* What a program shows: each key's commit, then the key if it passed. */
static char typed[256];

static void append(const char *s)
{
    size_t used = strlen(typed), n = strlen(s);
    if (used + n < sizeof typed) memcpy(typed + used, s, n + 1);
}

static void got(int taken, const char *as)
{
    const char *c = ime_get_commit();
    if (c) append(c);
    if (!taken) append(as);
}

/* A direct program's key (ime_process_key), and the bar's keysym
 * (ime_key_event) with its release. */
static void type_key(int key, const char *as)
{
    got(ime_process_key(key), as);
}

static void type_sym(unsigned int sym, const char *as)
{
    got(ime_key_event(sym, 0), as);
    ime_key_event(sym, 1u << 30);
    got(1, "");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fputs("usage: test-ime RIME_USER_DIR [DEVICE]\n", stderr);
        return 2;
    }
    struct libinput *li = libinput_path_create_context(&iface, NULL);
    if (!li || (argc == 3 && !libinput_path_add_device(li, argv[2]))) {
        fputs("FAIL libinput: no context or device\n", stderr);
        return 1;
    }
    libinput_dispatch(li);
    puts("PASS libinput: a context and a device");
    if (ime_init(NULL, argv[1]) != 0) {
        fputs("FAIL ime_init\n", stderr);
        fail();
    }
    ime_set_mode(IME_MODE_CN);
    ime_process_key('n');
    ime_process_key('i');
    char row1[1024], row2[1024];
    if (!ime_get_overlay(row1, row2, sizeof row1) || strstr(row1, "1") == NULL) {
        fprintf(stderr, "FAIL no candidates for ni: [%s]\n", row1);
        fail();
    }
    printf("PASS Rime's Chinese schema with its Lua: ni gives %.60s\n", row1);
    const char *chosen = ime_process_key('1') ? ime_get_commit() : NULL;
    if (!chosen || !*chosen) {
        fputs("FAIL 1 does not choose the first candidate of ni\n", stderr);
        fail();
    }
    printf("PASS 1 chooses it: %s\n", chosen);

    static const struct { int key; const char *text; } table[] = {
        { ',', "，" }, { '.', "。" }, { '<', "《" }, { '>', "》" }, { '?', "？" }, { ';', "；" },
        { ':', "：" }, { '\'', "‘" }, { '\'', "’" }, { '"', "“" }, { '"', "”" }, { '\\', "、" },
        { '!', "！" }, { '$', "¥" }, { '^', "……" }, { '(', "（" }, { ')', "）" }, { '_', "——" },
        { '[', "【" }, { ']', "】" }, { '{', "「" }, { '}', "」" },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        int taken = ime_process_key(table[i].key);
        const char *text = ime_get_commit();
        if (!taken || !text || strcmp(text, table[i].text) != 0) {
            fprintf(stderr, "FAIL full width %c: taken %d, gives [%s], not %s\n", table[i].key, taken,
                    text ? text : "", table[i].text);
            fail();
        }
    }
    static const char as_typed[] = "/|~`*&@#%-+=";
    for (const char *k = as_typed; *k; k++) {
        if (ime_process_key(*k) || ime_get_commit() || ime_get_overlay(row1, row2, sizeof row1)) {
            fprintf(stderr, "FAIL full width %c is not left as typed\n", *k);
            fail();
        }
    }
    puts("PASS full width at first: ，。《》？；：‘’“”、！¥……（）——【】「」 for their keys, the others as typed");
    static const struct { int key, taken; const char *text; } after_digit[] = {
        { '3', 0, NULL }, { '.', 1, NULL }, { '1', 1, ".1" }, { '4', 0, NULL },
        { '2', 0, NULL }, { ':', 1, NULL }, { '3', 1, ":3" }, { '0', 0, NULL },
        { '7', 0, NULL }, { '.', 1, NULL }, { ' ', 1, ". " },
        { '5', 0, NULL }, { '.', 1, NULL }, { '.', 1, "。" },
        { 'n', 1, NULL }, { 'i', 1, NULL }, { '1', 1, "" }, { '.', 1, "。" },
        { '6', 0, NULL }, { ',', 1, "，" },
    };
    for (size_t i = 0; i < sizeof after_digit / sizeof after_digit[0]; i++) {
        int taken = ime_process_key(after_digit[i].key);
        const char *text = ime_get_commit();
        const char *want = after_digit[i].text;
        if (taken != after_digit[i].taken || (want == NULL) != (text == NULL) ||
            (want && *want && strcmp(text, want) != 0)) {
            fprintf(stderr, "FAIL after digits, key %d (%c): taken %d, gives [%s], not %d [%s]\n", (int)i,
                    after_digit[i].key, taken, text ? text : "(nothing)", after_digit[i].taken, want ? want : "(nothing)");
            fail();
        }
    }
    puts("PASS after a digit . and : as typed (3.14, 12:30, 7. ), the key again 。; not after one 。 and ，");
    /* Caps Lock as Rime's front ends give it (good_old_caps_lock): locking it
     * ends the composition; locked, keys pass by Rime (the program types
     * them, in capitals); unlocked, pinyin again. */
    ime_process_key('n');
    ime_caps_key(1, 0);
    ime_set_caps(1);
    ime_caps_key(0, 1);
    if (ime_get_overlay(row1, row2, sizeof row1) || ime_process_key('N') || ime_process_key('I') ||
        ime_process_key(',') || ime_get_commit() || ime_get_overlay(row1, row2, sizeof row1)) {
        fputs("FAIL Caps Lock locked: the composition stays, or keys do not pass by Rime\n", stderr);
        fail();
    }
    ime_caps_key(1, 1);
    ime_set_caps(0);
    ime_caps_key(0, 0);
    ime_process_key('n');
    ime_process_key('i');
    if (!ime_get_overlay(row1, row2, sizeof row1) || strstr(row1, "1") == NULL) {
        fprintf(stderr, "FAIL Caps Lock unlocked: no candidates for ni: [%s]\n", row1);
        fail();
    }
    ime_process_key(27);
    ime_get_commit();
    puts("PASS Caps Lock: locking ends the composition, locked keys pass by Rime, unlocked pinyin again");
    /* Shift_L, Shift_R, Eisu_toggle and F4 must not flip Rime's ascii_mode:
     * the production schemas have no such switch keys, and ime_key_event keeps
     * ascii_mode off around every key. */
    static const struct { unsigned int sym, mask; } internal_toggles[] = {
        { 0xffe1, 0 }, { 0xffe1, (1u << 0) | (1u << 30) }, /* Shift_L tap */
        { 0xffe2, 0 }, { 0xffe2, (1u << 0) | (1u << 30) }, /* Shift_R tap */
        { 0xff30, 0 }, { 0xff30, 1u << 30 },               /* JIS Eisu_toggle */
        { 0xffc1, 0 }, { 0xffc1, 1u << 30 },               /* F4 switcher */
        { '2', (1u << 2) | (1u << 0) }, { '2', (1u << 2) | (1u << 0) | (1u << 30) },
        { '@', (1u << 2) | (1u << 0) }, { '@', (1u << 2) | (1u << 0) | (1u << 30) },
    };
    for (size_t i = 0; i < sizeof internal_toggles / sizeof internal_toggles[0]; i++) {
        ime_key_event(internal_toggles[i].sym, internal_toggles[i].mask);
        ime_get_commit();
    }
    if (ime_get_overlay(row1, row2, sizeof row1)) {
        fprintf(stderr, "FAIL unexpected switcher/preedit overlay: [%s]\n", row1);
        fail();
    }
    ime_process_key('n');
    ime_process_key('i');
    if (!ime_get_overlay(row1, row2, sizeof row1) || strstr(row1, "1") == NULL) {
        fprintf(stderr, "FAIL Shift/Eisu/F4 flipped ascii_mode: no candidates for ni: [%s]\n", row1);
        fail();
    }
    ime_process_key(27);
    ime_get_commit();
    if (!ime_process_key(',') || ! (chosen = ime_get_commit()) || strcmp(chosen, "，") != 0) {
        fputs("FAIL Shift/Eisu/F4 left Chinese punctuation in ASCII mode\n", stderr);
        fail();
    }
    puts("PASS Shift_L, Shift_R, Eisu_toggle and F4 never flip Rime into internal ascii_mode");
    ime_toggle_width();
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (ime_process_key(table[i].key) || ime_get_commit()) {
            fprintf(stderr, "FAIL half width %c is not left as typed\n", table[i].key);
            fail();
        }
    }
    puts("PASS half width: every key as typed");

    /* English mode with the real easy_en (this tree's schema, the runtime's
     * words): nothing typed is lost or changed without Tab (#291), through
     * the direct programs' entry and the bar's keysyms (keypad digits with
     * Num Lock on, KP_Enter). */
    static const char probe[] = "ab cd-ef 12 x3 qq\n";
    ime_set_mode(IME_MODE_EN);
    typed[0] = '\0';
    for (const char *k = probe; *k; k++) type_key(*k, (char[]){ *k, 0 });
    if (strcmp(typed, probe) != 0) {
        fprintf(stderr, "FAIL English, direct: \"%s\" typed as \"%s\"\n", probe, typed);
        fail();
    }
    typed[0] = '\0';
    for (const char *k = probe; *k; k++) {
        unsigned int sym = *k == '\n' ? 0xff0d : (unsigned char)*k;
        if (*k >= '0' && *k <= '9') type_key(*k, (char[]){ *k, 0 }); /* the bar's candidate path */
        else type_sym(sym, (char[]){ *k, 0 });
    }
    type_sym('k', "k");
    type_key('1', "1"); /* KP_1, Num Lock on: the bar's candidate path */
    type_sym(0xffb2, "2"); /* KP_2 as a keysym */
    type_sym(0xff8d, "\n"); /* KP_Enter */
    if (strcmp(typed, "ab cd-ef 12 x3 qq\nk12\n") != 0) {
        fprintf(stderr, "FAIL English, keysyms: typed as \"%s\"\n", typed);
        fail();
    }
    puts("PASS English: \"ab cd-ef 12 x3 qq\\n\" and keypad digits typed exactly; Space, -, digits and Return pass");
    /* Tab moves to a completion, Space commits it and is typed too. */
    typed[0] = '\0';
    type_key('a', "a");
    type_key('b', "b");
    char word[128] = "";
    int shown = ime_get_overlay(row1, row2, sizeof row1);
    if (!shown || strstr(row1, "1.") || strchr(strstr(row1, " | ") ? strstr(row1, " | ") : row1, '[')) {
        fprintf(stderr, "FAIL English overlay: digit labels or a chosen completion before Tab: [%s]\n", row1);
        fail();
    }
    type_key('\t', "\t");
    ime_get_overlay(row1, row2, sizeof row1);
    const char *open = strstr(row1, " | ") ? strchr(strstr(row1, " | "), '[') : NULL;
    if (open) sscanf(open + 1, "%127[^]]", word);
    type_key(' ', " ");
    char want[160];
    snprintf(want, sizeof want, "%s ", word);
    if (!word[0] || strcmp(typed, want) != 0) {
        fprintf(stderr, "FAIL English Tab: chose [%s], typed \"%s\"\n", word, typed);
        fail();
    }
    printf("PASS English: Tab moves to a completion (%s), Space commits it and is typed\n", word);
    ime_done();
    libinput_unref(li);
    puts("ALL PASSED");
    return 0;
}
