/* keys.c's name of the last key (settings saves the terminal key under it):
 * the modifiers held and the key as rc.conf names keys; programs still get
 * the key itself. Key events through a pipe; no device. Build with
 * -ffunction-sections -fdata-sections -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define KEYS_TABLE "/nonexistent/keymap.table" /* US: no system's table */
#include "../base/keys.c"
#include <assert.h>

static const char *rc[5]; /* rc.conf's NAME, VALUE pairs; none: the defaults */
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    for (int i = 0; rc[i]; i += 2) fn(rc[i], rc[i + 1], arg);
    return 0;
}
void say(const char *fmt, ...) { (void)fmt; }

static int pipe_fds[2];

/* The input method's side (ime.c): what Caps Lock told it. */
static char caps_log[128];
void ime_caps_key(int pressed, int locked)
{
    size_t n = strlen(caps_log);
    snprintf(caps_log + n, sizeof caps_log - n, "%s%d ", pressed ? "press" : "release", locked);
}
void ime_set_caps(int locked)
{
    size_t n = strlen(caps_log);
    snprintf(caps_log + n, sizeof caps_log - n, "now%d ", locked);
}

static int key(int code, int value)
{
    struct input_event ev = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(pipe_fds[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    return keys_event(pipe_fds[0]);
}

int main(void)
{
    char name[64];
    assert(pipe(pipe_fds) == 0);
    assert(key(KEY_LEFTCTRL, 1) == 0 && key(KEY_LEFTALT, 1) == 0);
    assert(key(KEY_T, 1) == ('t' & 0x1F));
    assert(keys_last_name(name, sizeof name) == 0 && strcmp(name, "Ctrl+Alt+T") == 0);
    assert(key(KEY_T, 0) == 0 && key(KEY_LEFTALT, 0) == 0);
    assert(key(KEY_LEFTBRACE, 1) == 27 && keys_last_name(name, sizeof name) < 0); /* Ctrl+[: no name */
    assert(key(KEY_LEFTBRACE, 0) == 0 && key(KEY_LEFTCTRL, 0) == 0);
    assert(key(KEY_LEFTSHIFT, 1) == 0 && key(KEY_LEFTMETA, 1) == 0);
    assert(key(KEY_ENTER, 1) == '\n');
    assert(keys_last_name(name, sizeof name) == 0 && strcmp(name, "Shift+Super+Enter") == 0);
    assert(key(KEY_ENTER, 0) == 0 && key(KEY_LEFTSHIFT, 0) == 0);
    assert(key(KEY_7, 1) == '7' && keys_last_name(name, sizeof name) == 0 && strcmp(name, "Super+7") == 0);
    assert(key(KEY_7, 0) == 0 && key(KEY_LEFTMETA, 0) == 0);
    assert(key(KEY_SPACE, 1) == ' ' && keys_last_name(name, sizeof name) == 0 && strcmp(name, "Space") == 0);
    puts("PASS a key with its modifiers named as rc.conf names keys (Ctrl+Alt+T); others have none");
    assert(key(KEY_SPACE, 0) == 0 && key(KEY_LEFTMETA, 1) == 0);
    assert(key(KEY_L, 1) == K_ACT_LOCK && key(KEY_L, 2) == 0); /* held: once */
    assert(key(KEY_L, 0) == 0 && key(KEY_LEFTMETA, 0) == 0);
    puts("PASS key_lock is Super+L unless rc.conf says otherwise, once however long it is held");
    /* Caps Lock: letters in capitals (with Shift small), not digits or
     * punctuation, Ctrl+letter as ever; the input method told the key with
     * the lock before it, then the lock after. */
    assert(key(KEY_CAPSLOCK, 1) == 0 && key(KEY_CAPSLOCK, 2) == 0 && key(KEY_CAPSLOCK, 0) == 0);
    assert(strcmp(caps_log, "press0 now1 release1 ") == 0);
    assert(key(KEY_A, 1) == 'A' && key(KEY_A, 0) == 0 && key(KEY_1, 1) == '1' && key(KEY_1, 0) == 0);
    assert(key(KEY_SEMICOLON, 1) == ';' && key(KEY_SEMICOLON, 0) == 0);
    assert(key(KEY_LEFTSHIFT, 1) == 0 && key(KEY_A, 1) == 'a' && key(KEY_1, 1) == '!');
    assert(key(KEY_A, 0) == 0 && key(KEY_1, 0) == 0 && key(KEY_LEFTSHIFT, 0) == 0);
    assert(key(KEY_LEFTCTRL, 1) == 0 && key(KEY_C, 1) == ('c' & 0x1F) && key(KEY_C, 0) == 0 && key(KEY_LEFTCTRL, 0) == 0);
    caps_log[0] = '\0';
    assert(key(KEY_CAPSLOCK, 1) == 0 && key(KEY_CAPSLOCK, 0) == 0);
    assert(strcmp(caps_log, "press1 now0 release0 ") == 0);
    assert(key(KEY_A, 1) == 'a' && key(KEY_A, 0) == 0);
    puts("PASS Caps Lock: letters in capitals (Shift small), digits and Ctrl as ever; the input method told");
    /* key_home: ThinkVantage (KEY_VENDOR, KEY_PROG1) goes home, whatever is
     * held, unless rc.conf names other keys; then those do, it nothing. */
    assert(key(KEY_VENDOR, 1) == K_ACT_HOME && key(KEY_VENDOR, 0) == 0 && keys_alone_action(KEY_PROG1) == K_ACT_HOME);
    assert(keys_alone_action(KEY_MICMUTE) == K_MIC_MUTE && keys_alone_action(KEY_ESC) == 0);
    rc[0] = "key_home", rc[1] = "Super+Esc MicMute", rc[2] = "key_mic_mute", rc[3] = "";
    keys_reload();
    assert(key(KEY_VENDOR, 1) == 0 && key(KEY_VENDOR, 0) == 0 && key(KEY_PROG1, 1) == 0 && key(KEY_PROG1, 0) == 0);
    assert(keys_alone_action(KEY_VENDOR) == 0 && keys_alone_action(KEY_PROG1) == 0);
    assert(key(KEY_LEFTCTRL, 1) == 0 && key(KEY_MICMUTE, 1) == K_ACT_HOME && key(KEY_MICMUTE, 0) == 0);
    assert(key(KEY_LEFTCTRL, 0) == 0 && keys_alone_action(KEY_MICMUTE) == K_ACT_HOME);
    assert(key(KEY_LEFTMETA, 1) == 0 && key(KEY_ESC, 1) == K_ACT_HOME && key(KEY_ESC, 0) == 0 && key(KEY_LEFTMETA, 0) == 0);
    puts("PASS key_home=\"Super+Esc MicMute\": MicMute goes home (with Ctrl held too), Super+Esc still; ThinkVantage nothing");
    puts("ALL PASSED");
    return 0;
}
