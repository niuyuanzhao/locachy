/* Real bar keyboard event loop with pipe queues and XKB, no display/input. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
static int fake_audio(int, char *, size_t);
static int fake_brightness(int, char *, size_t);
static int fake_signal(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
#define kill fake_signal
#define base_audio_key fake_audio
#define base_brightness_key fake_brightness
#ifdef CONFIG_IME
#define ime_get_commit typed_commit /* the bar's commits, seen here */
#endif
#define main bar_main
#include "../progs/bar.c"
#undef main
#ifdef CONFIG_IME
#undef ime_get_commit
#endif
#undef base_audio_key
#undef base_brightness_key
#undef kill
#include "i18n-stub.h"

#ifdef CONFIG_IME
/* The last text Rime committed, as the bar typed it. */
static char committed[64];
const char *ime_get_commit(void);
const char *typed_commit(void)
{
    const char *c = ime_get_commit();
    if (c && c[0]) snprintf(committed, sizeof committed, "%s", c);
    return c;
}
#endif

static int audio_action, brightness_action;
static int fake_audio(int key, char *out, size_t size)
{
    audio_action = key;
    (void)out;
    (void)size;
    return -1;
}
static int fake_brightness(int key, char *out, size_t size)
{
    brightness_action = key;
    (void)out;
    (void)size;
    return -1;
}
void say(const char *fmt, ...) { (void)fmt; }
static const char *rc_width_cn = NULL; /* rc.conf's key_ime_width_cn, when it has one */
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    if (rc_width_cn) fn("key_ime_width_cn", rc_width_cn, arg);
    return 0;
}
double uptime(void) { return 1.0; }
int text_px(double scale) { (void)scale; return 18; }
const struct panel_font *panel_font(int height, int bold) { (void)height; (void)bold; return NULL; }
void panel_font_size(const struct panel_font *font, int *w, int *h) { (void)font; *w = 9; *h = 18; }
void panel_fill(uint32_t *px, int w, int h, int stride, int x, int y, int bw, int bh, uint32_t color)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)bw;(void)bh;(void)color; }
void panel_text(uint32_t *px, int w, int h, int stride, int x, int y, const char *text,
                uint32_t fg, uint32_t bg, const struct panel_font *font)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)text;(void)fg;(void)bg;(void)font; }
int panel_text_width(const char *text, const struct panel_font *font) { (void)font; return (int)strlen(text) * 9; }
size_t panel_text_cut(const char *text, int width, const struct panel_font *font)
{ (void)font; size_t n = (size_t)(width > 0 ? width / 9 : 0); return strlen(text) < n ? strlen(text) : n; }
void panel_text_fit(char *out, size_t size, const char *text, int width, const struct panel_font *font)
{ (void)width;(void)font;snprintf(out,size,"%.*s",(int)(size ? size-1 : 0),text); }
void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *x0, int *x1)
{ (void)px;(void)w;(void)h;(void)stride;(void)title;(void)sub;(void)ime;(void)reserve;*x0=0;*x1=0; }
int panel_containers(const char *name, unsigned long long *ram) { (void)name;*ram=0;return 0; }
int open_containers(char names[][OPEN_NAME], int max) { (void)names;(void)max;return 0; }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type;(void)names;(void)max;return 0; }
int open_rule(const char *type, char *name, size_t size) { (void)type;(void)name;(void)size;return OPEN_NO_RULE; }
int open_set_rule(const char *type, const char *value) { (void)type;(void)value;return 0; }
void open_label(const char *name, const char *type, char *out, size_t size)
{ (void)type;snprintf(out,size,"%s",name); }
int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{ (void)name;(void)target;(void)spec;(void)size;(void)argv;(void)max;return 0; }
int open_shared(const char *path, char *why, size_t size) { (void)path;(void)why;(void)size;return 0; }
int open_host(const char *name) { (void)name;return 0; }
int open_in_home(const char *with, const char *target) { (void)with;(void)target;return 0; }

static int events[2];
#ifdef CONFIG_IME
static void write_schema(const char *directory)
{
    char path[PATH_MAX];
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof path, "%s/default.yaml", directory);
    FILE *f = fopen(path, "w");
    assert(f && fputs("config_version: '1'\nschema_list:\n  - schema: easy_en\n  - schema: rime_frost\n"
                      "ascii_composer:\n  good_old_caps_lock: true\n  switch_key:\n    Caps_Lock: clear\n", f) >= 0 && fclose(f) == 0);
    static const char *const schemas[] = { "easy_en", "rime_frost" };
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s.schema.yaml", directory, schemas[i]);
        f = fopen(path, "w");
        assert(f);
        fprintf(f, "schema:\n  schema_id: %s\n  name: Test\n  version: '1'\n"
                "switches:\n  - name: ascii_mode\n    reset: 0\n"
                "engine:\n  processors: [ascii_composer, recognizer, speller, selector, express_editor]\n"
                "  segmentors: [ascii_segmentor, matcher, abc_segmentor, fallback_segmentor]\n"
                "  translators: [table_translator]\n"
                "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n  delimiter: \" '\"\n"
                "translator:\n  dictionary: test\n  enable_user_dict: false\n", schemas[i]);
        assert(fclose(f) == 0);
    }
    snprintf(path, sizeof path, "%s/test.dict.yaml", directory);
    f = fopen(path, "w");
    assert(f && fputs("---\nname: test\nversion: '1'\nsort: by_weight\n...\n\xe4\xbd\xa0\xe5\xa5\xbd\tnihao\t100\nhello\thello\t100\n", f) >= 0 && fclose(f) == 0);
}
#endif

static void integration_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "zwlr_layer_shell_v1")) layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, "wl_output")) {
        struct bar *b = calloc(1, sizeof *b);
        assert(b);
        b->name = name;
        b->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
        b->next = bars; bars = b;
    }
    else if (!strcmp(iface, "wl_seat")) wl_seat_obj = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1"))
        vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void integration_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }

static void key(int code, int value)
{
    struct input_event e = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(events[1], &e, sizeof e) == sizeof e);
    handle_kbd_event(events[0]);
}

/* A key of the second keyboard (other: its pipe). */
static int other[2];
static void okey(int code, int value)
{
    struct input_event e = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(other[1], &e, sizeof e) == sizeof e);
    handle_kbd_event(other[0]);
}

static int sym(int l, int code)
{
    return (int)xkb_state_key_get_one_sym(layouts[l].st, (xkb_keycode_t)code + 8);
}

static int mod(int l, const char *name, enum xkb_state_component kind)
{
    return xkb_state_mod_name_is_active(layouts[l].st, name, kind) > 0;
}

static int nothing_fed(void)
{
    for (int l = 0; l < nlayouts; l++) {
        for (int c = 0; c <= KEY_MAX; c++) if (layouts[l].fed[c]) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int integration = argc >= 2 && !strcmp(argv[1], "--headless");
    assert(argc == 1 || integration);
#ifdef CONFIG_IME
    if (integration) {
        assert(argc == 4);
        write_schema(argv[2]);
        assert(mkdir(argv[3], 0700) == 0);
        assert(ime_init(argv[2], argv[3]) == 0);
    }
#endif
    if (integration) {
        display = wl_display_connect(NULL);
        assert(display);
        registry = wl_display_get_registry(display);
        static const struct wl_registry_listener listener = { integration_global, integration_remove };
        wl_registry_add_listener(registry, &listener, NULL);
        assert(wl_display_roundtrip(display) >= 0 && wl_display_roundtrip(display) >= 0);
        assert(vkbd_mgr && wl_seat_obj);
        text_vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, wl_seat_obj);
    }
    assert(pipe2(events, O_NONBLOCK | O_CLOEXEC) == 0);
    nkbds = 1;
    kbds[0] = events[0];
    active = 1;
    want_grab = grabbed = 0;
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(xkb_ctx);
    /* The system's key table names that layout (keys.c), as keytable writes
     * them, with its letters, digits and space: the keyboards without one of
     * their own type with it, the input method gets them through it. */
    char table[] = "/tmp/bar-keyboard-table-XXXXXX";
    int tfd = mkstemp(table);
    FILE *tf = tfd >= 0 ? fdopen(tfd, "w") : NULL;
    assert(tf);
    fputs("console jp106\nlayout jp\nmodel jp106\nvariant -\noptions ctrl:swapcaps\n", tf);
    static const struct { int first; const char *keys; } rows[] = {
        { KEY_1, "1234567890" }, { KEY_Q, "qwertyuiop" }, { KEY_A, "asdfghjkl" }, { KEY_Z, "zxcvbnm" }, { KEY_SPACE, " " },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        for (int i = 0; rows[r].keys[i]; i++) {
            int c = rows[r].keys[i], upper = c >= 'a' && c <= 'z' ? c - 32 : c;
            char name[8];
            snprintf(name, sizeof name, "%s", c == ' ' ? "space" : (char[]){ (char)c, 0 });
            fprintf(tf, "key %d %s", rows[r].first + i, name);
            for (int st = 0; st < 8; st++) {
                int ch = !!(st & 1) != !!(st & 2) ? upper : c; /* Shift, Caps Lock: letters only */
                fprintf(tf, " %x:%x", ch, ch);
            }
            fputc('\n', tf);
        }
    }
    assert(fclose(tf) == 0);
    assert(keys_layout_load(table) == 0 && unlink(table) == 0);
    assert(keys_char(KEY_N, 0) == 'n' && keys_char(KEY_SPACE, 0) == ' ');
    const char *const jp_swap[4] = { "jp", "jp106", NULL, "ctrl:swapcaps" };
    assert(layout_init(&layouts[0], jp_swap) == 0);
    nlayouts = 1;
    key(KEY_CAPSLOCK, 1);
    assert(ctrl && !caps_locked());
    key(KEY_C, 1);
    assert(ctrl);
    key(KEY_C, 0);
    key(KEY_CAPSLOCK, 0);
    assert(!ctrl && !caps_locked());
    key(KEY_LEFTCTRL, 1);
    key(KEY_LEFTCTRL, 0);
    assert(!ctrl && caps_locked());
    key(KEY_LEFTCTRL, 1);
    key(KEY_LEFTCTRL, 0);
    assert(!caps_locked());
    puts("PASS graphical shortcuts track XKB Ctrl/Caps roles, not physical positions");
    assert(pipe2(other, O_NONBLOCK | O_CLOEXEC) == 0);
    nkbds = 2; kbds[1] = other[0];
    key(KEY_CAPSLOCK, 1);
    struct input_event both = { .type = EV_KEY, .code = KEY_CAPSLOCK, .value = 1 };
    assert(write(other[1], &both, sizeof both) == sizeof both);
    handle_kbd_event(other[0]);
    key(KEY_CAPSLOCK, 0);
    assert(ctrl && layouts[0].fed[KEY_CAPSLOCK] == 1);
    both.value = 0;
    assert(write(other[1], &both, sizeof both) == sizeof both);
    handle_kbd_event(other[0]);
    assert(!ctrl && keys_down == 0);
    puts("PASS identical modifier keycodes on two devices remain held until both release");
    key(KEY_CAPSLOCK, 1);
    both.value = 1;
    assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
    reconcile_keyboards(&other[0], 1);
    assert(nkbds == 1 && kbds[0] == other[0] && ctrl);
    both.value = 0;
    assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
    assert(!ctrl);
    int reconnect[] = { events[0], other[0] };
    reconcile_keyboards(reconnect, 2);
    assert(nkbds == 2 && keys_down == 0);
    puts("PASS removing one keyboard preserves another device's held modifier; reconnect needs no desktop restart");

    /* The second keyboard with a layout of its own (us) beside the system's
     * (jp, Ctrl and Caps Lock swapped): each key as its keyboard's layout
     * has it, the modifiers of both keyboards in both, one Caps Lock. */
    const char *const us[4] = { "us", "pc105", NULL, NULL };
    assert(layout_init(&layouts[1], us) == 0);
    nlayouts = 2;
    kbd_layout[1] = 1;
    assert(grab_wanted()); /* in direct input too: the compositor has one layout */
    key(KEY_LEFTSHIFT, 1);
    assert(sym(1, KEY_2) == XKB_KEY_at && sym(0, KEY_2) == XKB_KEY_quotedbl);
    okey(KEY_2, 1); okey(KEY_2, 0);
    key(KEY_LEFTSHIFT, 0);
    assert(sym(1, KEY_2) == XKB_KEY_2 && !shift);
    key(KEY_CAPSLOCK, 1); /* jp's Ctrl here */
    okey(KEY_C, 1);
    assert(ctrl && mod(1, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE));
    okey(KEY_C, 0);
    key(KEY_CAPSLOCK, 0);
    assert(!mod(1, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) && !mod(0, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE));
    key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* jp's Caps Lock */
    assert(mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    assert(sym(1, KEY_A) == XKB_KEY_A);
    okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0); /* us's Caps Lock unlocks both */
    assert(!mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && !mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    /* jp's own Caps Lock is Shift+Eisu: its lock is the others' too. */
    const char *const jp_plain[4] = { "jp", "jp106", NULL, NULL };
    assert(layout_init(&layouts[2], jp_plain) == 0);
    nlayouts = 3;
    kbd_layout[1] = 2;
    okey(KEY_LEFTSHIFT, 1); okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0); okey(KEY_LEFTSHIFT, 0);
    assert(mod(2, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) &&
           mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* unlocked by the other keyboard's */
    assert(!mod(2, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && !mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    kbd_layout[1] = 1;
    okey(KEY_LEFTSHIFT, 1);
    assert(shift && sym(0, KEY_2) == XKB_KEY_quotedbl);
    int jp_only[] = { events[0] };
    reconcile_keyboards(jp_only, 1); /* the us keyboard gone with Shift held */
    assert(!mod(0, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) && nothing_fed() && !grab_wanted());
    reconcile_keyboards(reconnect, 2);
    kbd_layout[1] = 1;
    puts("PASS a keyboard of its own layout: its keys by it, modifiers of both keyboards in both, one Caps Lock, "
         "unplugged held keys released");
    /* Chinese mode's width keys, rc.conf's key_ime_width_cn: Ctrl+. and
     * Shift+Space unless set, each key by its symbol in its keyboard's
     * layout (jp's @ is a key of its own, us's is Shift+2). */
    load_width_keys();
    key(KEY_CAPSLOCK, 1); /* jp's Ctrl here */
    assert(width_key(0, KEY_DOT) && width_key(1, KEY_DOT) && !width_key(0, KEY_SPACE));
    key(KEY_CAPSLOCK, 0);
    okey(KEY_LEFTSHIFT, 1);
    assert(width_key(1, KEY_SPACE) && width_key(0, KEY_SPACE) && !width_key(1, KEY_DOT));
    okey(KEY_LEFTSHIFT, 0);
    assert(!width_key(0, KEY_DOT) && !width_key(0, KEY_SPACE) && !width_key(1, KEY_SPACE));
    rc_width_cn = "Ctrl+@,Hiragana_Katakana";
    load_width_keys();
    okey(KEY_LEFTCTRL, 1);
    assert(!width_key(0, KEY_DOT) && width_key(0, KEY_LEFTBRACE) && !width_key(1, KEY_LEFTBRACE) &&
           !width_key(1, KEY_2));
    okey(KEY_LEFTCTRL, 0);
    assert(width_key(0, KEY_KATAKANAHIRAGANA) && !width_key(0, KEY_LEFTBRACE));
    rc_width_cn = "";
    load_width_keys();
    assert(nwidth_keys == 0);
    rc_width_cn = NULL;
    load_width_keys();
    assert(nwidth_keys == 2 && keys_down == 0);
    puts("PASS Chinese mode's width keys from rc.conf's key_ime_width_cn (Ctrl+. and Shift+Space unless set), "
         "by each keyboard's layout");
    kbd_layout[1] = 0;
    if (integration) {
        init_xkb();
        assert(nlayouts == 1 && layouts[0].keymap_fd >= 0 && !strcmp(layouts[0].names[3], "ctrl:swapcaps"));
        restore_passthrough_keymap();
        assert(wl_display_roundtrip(display) >= 0);
        grabbed = want_grab = 1;
#ifdef CONFIG_IME
        ime_set_mode(IME_MODE_CN);
        static const int pinyin[] = { KEY_N, KEY_I, KEY_H, KEY_A, KEY_O, KEY_SPACE };
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            key(pinyin[i], 1); key(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        /* The same on the second keyboard: every keyboard's keys reach Rime. */
        committed[0] = '\0';
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            okey(pinyin[i], 1); okey(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        /* Chinese mode's width by key_ime_width_cn's keys, on either
         * keyboard; the keys are the bar's, not the application's. */
        char full[40];
        snprintf(full, sizeof full, "%s", ime_mode_label());
        key(KEY_CAPSLOCK, 1); key(KEY_DOT, 1); /* jp's Ctrl here */
        assert(strcmp(ime_mode_label(), full) != 0 && !layouts[0].down[KEY_DOT]);
        key(KEY_DOT, 0); key(KEY_CAPSLOCK, 0);
        okey(KEY_LEFTSHIFT, 1); okey(KEY_SPACE, 1);
        assert(strcmp(ime_mode_label(), full) == 0 && !layouts[0].down[KEY_SPACE]);
        okey(KEY_SPACE, 0); okey(KEY_LEFTSHIFT, 0);
        ime_set_mode(IME_MODE_EN);
        static const int english[] = { KEY_H, KEY_E, KEY_L, KEY_L, KEY_O, KEY_SPACE };
        for (size_t i = 0; i < sizeof english / sizeof english[0]; i++) {
            key(english[i], 1); key(english[i], 0);
        }
        ime_set_mode(IME_MODE_RAW);
        puts("PASS real Rime CN/EN composition and commit executed from pipe keyboard input");
        puts("PASS real Rime pinyin on a second keyboard commits Chinese text; its width keys switch the width");
#endif
        key(KEY_A, 1);
        assert(layouts[0].down[KEY_A] == 1);
        vkbd_type_utf8("\xe4\xbd\xa0\xe5\xa5\xbd");
        assert(layouts[0].down[KEY_A] == 1);
        key(KEY_A, 0);
        key(KEY_C, 1);
        both.code = KEY_C; both.value = 1;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(layouts[0].down[KEY_C] == 2);
        key(KEY_C, 0);
        assert(layouts[0].down[KEY_C] == 1);
        both.value = 0;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(!layouts[0].down[KEY_C]);
        key(KEY_RIGHT, 1);
        assert(layouts[0].down[KEY_RIGHT]);
        assert(bars);
        uint32_t removed_name = bars->name;
        reg_remove(NULL, NULL, removed_name);
        assert(!layouts[0].down[KEY_RIGHT] && !keys_down);
        key(KEY_RIGHT, 0);
        key(KEY_D, 1); key(KEY_D, 0);
        both.code = KEY_E; both.value = 1;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(layouts[0].down[KEY_E] == 1);
        int remaining[] = { events[0] };
        reconcile_keyboards(remaining, 1);
        assert(!layouts[0].down[KEY_E]);
        reconcile_keyboards(reconnect, 2);
        assert(wl_display_roundtrip(display) >= 0);
        puts("PASS real virtual-keyboard delivery keeps text commit separate from held passthrough keys");
        /* The second keyboard us, its own, beside the system's (jp, Ctrl and
         * Caps Lock swapped): the focused client gets each key in its
         * keyboard's keymap, with the modifiers of both. */
        const char *const us_own[4] = { "us", "pc105", NULL, NULL };
        assert(layout_init(&layouts[nlayouts], us_own) == 0);
        kbd_layout[1] = nlayouts++;
        grabbed = want_grab = 1;
        key(KEY_LEFTSHIFT, 1);
        key(KEY_2, 1); key(KEY_2, 0);   /* the system's, jp: " */
        okey(KEY_2, 1); okey(KEY_2, 0); /* us: @ */
        key(KEY_LEFTSHIFT, 0);
        okey(KEY_LEFTCTRL, 1);
        key(KEY_C, 1); key(KEY_C, 0);   /* Ctrl of the us keyboard, c of the jp one */
        okey(KEY_LEFTCTRL, 0);
        key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* jp's Caps Lock */
        okey(KEY_A, 1); okey(KEY_A, 0); /* A on the us keyboard: one Caps Lock */
        okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0);
        okey(KEY_A, 1); okey(KEY_A, 0);
#ifdef CONFIG_IME
        /* Pinyin on that keyboard, by its own layout, commits too. */
        ime_set_mode(IME_MODE_CN);
        committed[0] = '\0';
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            okey(pinyin[i], 1); okey(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        ime_set_mode(IME_MODE_RAW);
        puts("PASS real Rime pinyin on a keyboard of its own layout commits Chinese text");
#endif
        assert(wl_display_roundtrip(display) >= 0);
        for (int l = 0; l < nlayouts; l++) {
            for (int c = 0; c <= KEY_MAX; c++) assert(!layouts[l].down[c]);
        }
        int one[] = { events[0] };
        reconcile_keyboards(one, 1);
        reconcile_keyboards(reconnect, 2);
        assert(wl_display_roundtrip(display) >= 0);
        puts("PASS real delivery through a second layout's virtual keyboard: its keymap, both keyboards' modifiers");
        grabbed = want_grab = 0;
        text_vkbd = NULL;
        for (int l = 0; l < MAX_LAYOUTS; l++) layouts[l].vkbd = NULL;
    }
    key(KEY_CAPSLOCK, 1);
    layouts[0].down[KEY_RIGHT] = 1;
    kbd_forwarded[0][KEY_RIGHT] = 1;
    reset_keyboard_state();
    assert(!ctrl && !keys_down && !layouts[0].down[KEY_RIGHT] && !kbd_forwarded[0][KEY_RIGHT]);
    assert(nothing_fed());
    close(other[0]); close(other[1]); nkbds = 1;
    puts("PASS output/seat state reset clears depressed modifiers and forwarded key ownership");
    key(KEY_BRIGHTNESSUP, 1);
    key(KEY_BRIGHTNESSUP, 0);
    assert(brightness_action == K_BRI_UP);
    key(KEY_VOLUMEDOWN, 1);
    key(KEY_VOLUMEDOWN, 0);
    assert(audio_action == K_VOL_DOWN);
    key(KEY_MUTE, 1);
    key(KEY_MUTE, 0);
    assert(audio_action == K_VOL_MUTE && keys_down == 0);
    puts("PASS graphical application mode dispatches Fn brightness, volume and mute");
    layouts[0].down[KEY_LEFTMETA] = 1;
    grabbed = 0;
    set_grab(0);
    assert(!layouts[0].down[KEY_LEFTMETA]);
    sig_release = 0;
    on_sigusr2(SIGUSR2);
    assert(sig_release && !active && !grabbed);
    clear_virtual_keys();
    assert(!layouts[0].down[KEY_LEFTMETA]);
    puts("PASS seat release schedules balanced virtual-key cleanup even after the physical grab ended");
    ime_done();
    for (int l = 0; l < nlayouts; l++) layout_free(&layouts[l]);
    xkb_context_unref(xkb_ctx);
    close(events[0]); close(events[1]);
    return 0;
}
