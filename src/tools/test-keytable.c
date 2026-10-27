/* The key tables keytable writes (libxkbcommon, systemd's kbd-model-map) as
 * keys.c types with them: US as before, JIS (jp106) with its own symbols and
 * the input method's keys by keysym name (and one bound to nothing for
 * settings to record), German letters where they are, an unknown keymap
 * refused, a missing table US. Key events through a pipe; no
 * device. Build with -ffunction-sections -fdata-sections -Wl,--gc-sections:
 *   test-keytable KEYTABLE NEW_DIR */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define KEYS_TABLE table_path
static char table_path[512];
#include "../base/keys.c"
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *rc_width = NULL; /* rc.conf's key_ime_width, when it has one */
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    if (rc_width) fn("key_ime_width", rc_width, arg);
    return 0;
}

static int pipe_fds[2];
static int key(int code, int value)
{
    struct input_event ev = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(pipe_fds[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    return keys_event(pipe_fds[0]);
}
static int tap(int code)
{
    int k = key(code, 1);
    assert(key(code, 0) == 0);
    return k;
}
/* A key with a modifier held (AltGr, Shift: pressed, the key, released). */
static int with(int modifier, int code)
{
    assert(key(modifier, 1) == 0);
    int k = tap(code);
    assert(key(modifier, 0) == 0);
    return k;
}
/* The key typed this text (K_TEXT, or the one ASCII character). */
static int types(int k, const char *text)
{
    const char *t = keys_typed(k);
    return t && strcmp(t, text) == 0 && (strlen(text) == 1 ? k == text[0] : k == K_TEXT);
}

static const char *keytable;
static char dir[256];
static int make(const char *keymap)
{
    snprintf(table_path, sizeof table_path, "%s/%s.table", dir, keymap);
    pid_t pid = fork();
    if (pid == 0) {
        execl(keytable, keytable, keymap, table_path, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    keys_reload(); /* the table and rc.conf's keys again, as after settings */
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    keytable = argv[1];
    snprintf(dir, sizeof dir, "%s", argv[2]);
    assert(mkdir(dir, 0700) == 0);
    assert(pipe(pipe_fds) == 0);
    char name[64];

    /* US: what keys.c typed before there were tables, key for key. */
    assert(make("us") == 0);
    for (int code = 1; code < (int)sizeof normal - 1; code++) {
        assert(keys_char(code, 0) == (unsigned char)normal[code]);
        assert(keys_char(code, 1) == (unsigned char)shifted[code]);
    }
    assert(!strcmp(keys_layout_name("layout"), "us") && !strcmp(keys_layout_name("console"), "us"));
    assert(keys_layout_name("options") == NULL); /* terminate:ctrl_alt_bksp dropped */
    /* A JIS keyboard with the US layout: Henkan keeps its name, the key
     * left of 1 types ` (Zenkaku_Hankaku is jp's). */
    assert(tap(KEY_GRAVE) == '`' && tap(KEY_HENKAN) == K_ACT_IME_CN);
    puts("PASS us: every key of the old table the same; kbd-model-map's model, terminate: dropped");

    /* JIS: its symbols where its keys print them, the keys of its own. */
    assert(make("jp106") == 0);
    assert(!strcmp(keys_layout_name("layout"), "jp") && !strcmp(keys_layout_name("model"), "jp106"));
    assert(key(KEY_LEFTSHIFT, 1) == 0);
    assert(tap(KEY_2) == '"' && tap(KEY_7) == '\'' && tap(KEY_APOSTROPHE) == '*' && tap(KEY_RO) == '_');
    assert(key(KEY_LEFTSHIFT, 0) == 0);
    assert(tap(KEY_LEFTBRACE) == '@' && tap(KEY_APOSTROPHE) == ':' && tap(KEY_EQUAL) == '^' &&
           tap(KEY_YEN) == '\\' && tap(KEY_RO) == '\\' && tap(KEY_A) == 'a');
    /* Henkan right of the space bar Chinese, Muhenkan left of it direct
     * input, Zenkaku_Hankaku (KEY_GRAVE on a JIS keyboard) the width: once
     * however long each is held. */
    assert(key(KEY_HENKAN, 1) == K_ACT_IME_CN && key(KEY_HENKAN, 2) == 0 && key(KEY_HENKAN, 0) == 0);
    assert(keys_last_name(name, sizeof name) == 0 && !strcmp(name, "Henkan_Mode"));
    assert(tap(KEY_MUHENKAN) == K_ACT_IME_RAW && keys_last_name(name, sizeof name) == 0 && !strcmp(name, "Muhenkan"));
    assert(tap(KEY_GRAVE) == K_ACT_IME_WIDTH && keys_last_name(name, sizeof name) == 0 &&
           !strcmp(name, "Zenkaku_Hankaku"));
    assert(tap(KEY_KATAKANAHIRAGANA) == 0 && keys_last_name(name, sizeof name) == 0 &&
           !strcmp(name, "Hiragana_Katakana")); /* nothing bound: free for settings */
    /* settings, waiting for a key to record: that one comes too; the bound
     * ones as they are, letters as letters, modifiers alone not at all. */
    keys_name_any(1);
    assert(tap(KEY_KATAKANAHIRAGANA) == K_NAMED && keys_last_name(name, sizeof name) == 0 &&
           !strcmp(name, "Hiragana_Katakana"));
    assert(tap(KEY_HENKAN) == K_ACT_IME_CN && tap(KEY_A) == 'a' && tap(KEY_LEFTSHIFT) == 0);
    keys_name_any(0);
    assert(tap(KEY_KATAKANAHIRAGANA) == 0);
    assert(key(KEY_LEFTMETA, 1) == 0 && tap(KEY_L) == K_ACT_LOCK && key(KEY_LEFTMETA, 0) == 0);
    assert(key(KEY_LEFTCTRL, 1) == 0 && tap(KEY_SPACE) == K_CTRL_SPACE && key(KEY_LEFTCTRL, 0) == 0);
    rc_width = "Shift+Space Hiragana_Katakana";
    keys_reload();
    assert(tap(KEY_KATAKANAHIRAGANA) == K_ACT_IME_WIDTH && tap(KEY_GRAVE) == 0);
    assert(key(KEY_LEFTSHIFT, 1) == 0 && tap(KEY_SPACE) == K_ACT_IME_WIDTH && key(KEY_LEFTSHIFT, 0) == 0);
    rc_width = NULL;
    puts("PASS jp106: JIS symbols (Shift+2 \", @, :, ^, \\ twice), Henkan_Mode/Muhenkan/Zenkaku_Hankaku "
         "for the input method by keysym name, rc.conf's own keys instead");

    /* German: letters where its keys print them; bindings name letters. */
    assert(make("de") == 0);
    assert(tap(KEY_Y) == 'z' && tap(KEY_Z) == 'y');
    assert(key(KEY_LEFTMETA, 1) == 0 && tap(KEY_L) == K_ACT_LOCK && key(KEY_LEFTMETA, 0) == 0);
    /* Its characters beyond ASCII, with Shift and Caps Lock as xkb has them. */
    assert(types(tap(KEY_SEMICOLON), "ö") && types(with(KEY_LEFTSHIFT, KEY_APOSTROPHE), "Ä"));
    assert(types(tap(KEY_MINUS), "ß") && types(with(KEY_LEFTSHIFT, KEY_3), "§"));
    assert(tap(KEY_CAPSLOCK) == 0);
    assert(types(tap(KEY_APOSTROPHE), "Ä") && types(with(KEY_LEFTSHIFT, KEY_APOSTROPHE), "ä") && tap(KEY_Q) == 'Q');
    assert(tap(KEY_1) == '1' && tap(KEY_CAPSLOCK) == 0 && tap(KEY_Q) == 'q');
    /* AltGr (right Alt here): @ [ ] { } \ | ~ and €, and no Alt. */
    assert(with(KEY_RIGHTALT, KEY_Q) == '@' && with(KEY_RIGHTALT, KEY_8) == '[' && with(KEY_RIGHTALT, KEY_9) == ']');
    assert(with(KEY_RIGHTALT, KEY_7) == '{' && with(KEY_RIGHTALT, KEY_0) == '}' &&
           with(KEY_RIGHTALT, KEY_MINUS) == '\\' && with(KEY_RIGHTALT, KEY_102ND) == '|' &&
           with(KEY_RIGHTALT, KEY_RIGHTBRACE) == '~' && types(with(KEY_RIGHTALT, KEY_E), "€"));
    assert(key(KEY_RIGHTALT, 1) == 0 && !(keys_modifiers() & KEYS_ALT) && key(KEY_RIGHTALT, 0) == 0);
    assert(key(KEY_LEFTALT, 1) == 0 && tap(KEY_Q) == 'q' && (keys_modifiers() & KEYS_ALT) && key(KEY_LEFTALT, 0) == 0);
    /* Dead keys, as the compose table has them: ^ then a is â, then space
     * ^, twice ^; ´ then e é, Shift+´ (`) then a à; one that goes nowhere is
     * dropped with the key after it (libxkbcommon's cancel). */
    assert(tap(KEY_GRAVE) == 0 && types(tap(KEY_A), "â"));
    assert(tap(KEY_GRAVE) == 0 && tap(KEY_SPACE) == '^');
    assert(tap(KEY_GRAVE) == 0 && tap(KEY_GRAVE) == '^');
    assert(tap(KEY_EQUAL) == 0 && types(tap(KEY_E), "é"));
    assert(with(KEY_LEFTSHIFT, KEY_EQUAL) == 0 && types(tap(KEY_A), "à"));
    assert(tap(KEY_GRAVE) == 0 && tap(KEY_Q) == 0 && tap(KEY_Q) == 'q');
    assert(key(KEY_LEFTCTRL, 1) == 0 && tap(KEY_GRAVE) == 0 && tap(KEY_A) == 1 && key(KEY_LEFTCTRL, 0) == 0);
    assert(key(KEY_LEFTCTRL, 1) == 0 && tap(KEY_SEMICOLON) == 0 && key(KEY_LEFTCTRL, 0) == 0); /* Ctrl+ö: none */
    /* A keyboard of its own (per-device modifiers): AltGr the same. */
    int dev[2];
    assert(pipe(dev) == 0);
    keys_take(&dev[0], 1);
    struct input_event ev = { .type = EV_KEY, .code = KEY_RIGHTALT, .value = 1 };
    assert(write(dev[1], &ev, sizeof ev) == (ssize_t)sizeof ev && keys_event(dev[0]) == 0);
    ev.code = KEY_Q;
    assert(write(dev[1], &ev, sizeof ev) == (ssize_t)sizeof ev && keys_event(dev[0]) == '@');
    assert(!(keys_modifiers() & KEYS_ALT));
    keys_release(); /* closes dev[0] */
    close(dev[1]);
    puts("PASS de: QWERTZ letters; Super+L the key that types l; ä ö ß § with Shift and Caps Lock; AltGr's "
         "@ [ ] { } \\ | ~ € and no Alt; dead keys ^ ´ ` composed, cancelled as libxkbcommon does");

    /* British: £ on Shift+3 and € on AltGr+4, typed as they are (no $). */
    assert(make("uk") == 0);
    assert(types(with(KEY_LEFTSHIFT, KEY_3), "£") && with(KEY_LEFTSHIFT, KEY_4) == '$' &&
           types(with(KEY_RIGHTALT, KEY_4), "€"));
    /* French: é on 2, AltGr+0 @, ^ (its own dead key) then e ê. */
    assert(make("fr") == 0);
    assert(types(tap(KEY_2), "é") && with(KEY_LEFTSHIFT, KEY_2) == '2' && with(KEY_RIGHTALT, KEY_0) == '@');
    assert(tap(KEY_LEFTBRACE) == 0 && types(tap(KEY_E), "ê") && tap(KEY_RIGHTBRACE) == '$');
    /* US and JIS have no AltGr: right Alt is Alt. */
    assert(make("jp106") == 0);
    assert(key(KEY_RIGHTALT, 1) == 0 && (keys_modifiers() & KEYS_ALT) && tap(KEY_Q) == 'q' && key(KEY_RIGHTALT, 0) == 0);
    puts("PASS uk: £ and € as typed; fr: é, AltGr's @, its dead ^; jp106: right Alt is Alt");

    /* No layout: refused, the table there (German) kept; no table at all: US. */
    assert(make("de") == 0);
    char kept[512];
    snprintf(kept, sizeof kept, "%s", table_path);
    assert(make("no-such-keymap") == 1);
    assert(access(table_path, F_OK) != 0);
    snprintf(table_path, sizeof table_path, "%s", kept);
    keys_reload();
    assert(tap(KEY_Y) == 'z');
    snprintf(table_path, sizeof table_path, "%s/missing.table", dir);
    keys_reload();
    assert(tap(KEY_Y) == 'y' && tap(KEY_HENKAN) == 0 && keys_layout_name("layout") == NULL);
    assert(keys_layout_load(kept) == 0 && tap(KEY_Y) == 'z'); /* the installer's own */
    puts("PASS an unknown keymap is refused (exit 1, nothing written); without a table, US");
    return 0;
}
