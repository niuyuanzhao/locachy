/* A program as the base's programs are, opened as a window of a headless
 * labwc (LOCACHY_BACKEND=wayland): one line on stdout for each thing it
 * gets, for test-window-backend.sh. Its whole canvas is one colour and it
 * draws the top bar as programs do, so a screenshot finds the window and
 * shows whether the bar is there. k: answer the next closes with
 * key_close_kept(); u: no longer; q: end. */
#include <stdio.h>

#include "base.h"
#ifdef CONFIG_WAYLAND
#include "include/wayland-backend.h"
#endif

#ifdef WINDOW_INPUT_UNIT
#include <assert.h>
#include "../base/wayland-backend.c"

static struct pointer_event next_pointer(void)
{
    assert(pointer_next());
    return *pointer_last();
}

static struct base_raw_key next_raw(void)
{
    struct base_raw_key key;
    assert(base_raw_next(&key));
    return key;
}

static void expect_raw(uint32_t code, uint32_t state, xkb_keysym_t sym, uint32_t mods, uint32_t locks)
{
    struct base_raw_key key = next_raw();
    assert(key.device == BASE_RAW_WAYLAND_DEVICE && key.code == code && key.state == state);
    assert(key.keysym == sym && key.modifiers == mods && key.locks == locks);
}

static void test_raw_keyboard(xkb_mod_mask_t shift)
{
    const uint32_t press = WL_KEYBOARD_KEY_STATE_PRESSED, release = WL_KEYBOARD_KEY_STATE_RELEASED;
    keyboard_enter(NULL, NULL, 40, NULL, NULL);
    keyboard_modifiers(NULL, NULL, 41, 0, 0, 0, 0);
    base_raw_enable(1);

    keyboard_key(NULL, NULL, 42, 0, KEY_LEFTSHIFT, press);
    assert(keys_modifiers() == 0);
    expect_raw(KEY_LEFTSHIFT, 1, XKB_KEY_Shift_L, 0, 0);
    keyboard_modifiers(NULL, NULL, 43, shift, 0, 0, 0);
    keyboard_key(NULL, NULL, 44, 0, KEY_A, press);
    expect_raw(KEY_A, 1, XKB_KEY_A, KEYS_SHIFT, 0);
    assert(wl_backend_next_key() == 'A' && keys_modifiers() == KEYS_SHIFT);

    keyboard_key(NULL, NULL, 45, 0, KEY_LEFTSHIFT, release);
    expect_raw(KEY_LEFTSHIFT, 0, XKB_KEY_Shift_L, KEYS_SHIFT, 0);
    assert(keys_modifiers() == KEYS_SHIFT);
    /* Raw releases must not wait for the compositor mask to catch up. */
    keyboard_key(NULL, NULL, 46, 0, KEY_A, release);
    expect_raw(KEY_A, 0, XKB_KEY_A, 0, 0);
    keyboard_modifiers(NULL, NULL, 47, 0, 0, 0, 0);
    keyboard_key(NULL, NULL, 48, 0, KEY_A, press);
    expect_raw(KEY_A, 1, XKB_KEY_a, 0, 0);
    assert(wl_backend_next_key() == 'a' && keys_modifiers() == 0);
    keyboard_key(NULL, NULL, 49, 0, KEY_A, release);
    expect_raw(KEY_A, 0, XKB_KEY_a, 0, 0);
    assert(!base_raw_pending());
    puts("PASS raw Shift+A carries Shift; release clears it and preserves the press keysym");

    /* Re-enter with the same Shift still down. The compositor mask still
     * translates text, but the new raw session did not see its press. */
    keyboard_key(NULL, NULL, 50, 0, KEY_LEFTSHIFT, press);
    expect_raw(KEY_LEFTSHIFT, 1, XKB_KEY_Shift_L, 0, 0);
    keyboard_modifiers(NULL, NULL, 51, shift, 0, 0, 0);
    keyboard_leave(NULL, NULL, 52, NULL);
    assert(next_raw().state == 3);
    uint32_t held = KEY_LEFTSHIFT;
    struct wl_array keys = { .size = sizeof held, .data = &held };
    keyboard_enter(NULL, NULL, 53, NULL, &keys);
    keyboard_modifiers(NULL, NULL, 54, shift, 0, 0, 0);
    keyboard_key(NULL, NULL, 55, 0, KEY_A, press);
    expect_raw(KEY_A, 1, XKB_KEY_A, 0, 0);
    assert(wl_backend_next_key() == 'A' && keys_modifiers() == KEYS_SHIFT);
    keyboard_key(NULL, NULL, 56, 0, KEY_LEFTSHIFT, release);
    assert(!base_raw_pending());
    keyboard_modifiers(NULL, NULL, 57, 0, 0, 0, 0);
    keyboard_key(NULL, NULL, 58, 0, KEY_A, release);
    expect_raw(KEY_A, 0, XKB_KEY_A, 0, 0);
    puts("PASS Wayland focus changes cancel raw held keys without changing local text translation");

    xkb_mod_index_t caps = xkb_keymap_mod_get_index(w.keymap, XKB_MOD_NAME_CAPS);
    assert(caps < 32);
    keyboard_key(NULL, NULL, 59, 0, KEY_CAPSLOCK, press);
    expect_raw(KEY_CAPSLOCK, 1, XKB_KEY_Caps_Lock, 0, 0);
    keyboard_modifiers(NULL, NULL, 60, 1u << caps, 0, 1u << caps, 0);
    keyboard_key(NULL, NULL, 61, 0, KEY_CAPSLOCK, release);
    expect_raw(KEY_CAPSLOCK, 0, XKB_KEY_Caps_Lock, KEYS_CAPS, 1);
    keyboard_modifiers(NULL, NULL, 62, 0, 0, 1u << caps, 0);
    keyboard_key(NULL, NULL, 63, 0, KEY_A, press);
    expect_raw(KEY_A, 1, XKB_KEY_A, KEYS_CAPS, 1);
    assert(wl_backend_next_key() == 'A' && keys_modifiers() == 0);
    keyboard_leave(NULL, NULL, 64, NULL);
    assert(next_raw().state == 3);
    base_raw_enable(0);
    puts("PASS raw lock bits follow compositor masks without changing local modifiers");
}

int main(void)
{
    /* Exercise the real listeners with exact event ordering, including a
     * stop frame with no axis_source and a leave with a held button. */
    w.display = (struct wl_display *)&w; /* only wl_backend_active uses it */
    w.configured = 1;
    w.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(w.xkb);
    w.keymap = xkb_keymap_new_from_names(w.xkb, &(struct xkb_rule_names){ .layout = "us" },
                                         XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(w.keymap);
    w.state = xkb_state_new(w.keymap);
    assert(w.state);
    xkb_mod_index_t shift = xkb_keymap_mod_get_index(w.keymap, XKB_MOD_NAME_SHIFT);
    assert(shift < 32);
    test_raw_keyboard((xkb_mod_mask_t)1u << shift);
    keyboard_enter(NULL, NULL, 10, NULL, NULL);
    keyboard_modifiers(NULL, NULL, 11, 1u << shift, 0, 0, 0);
    pointer_motion(NULL, NULL, 0, wl_fixed_from_int(32), wl_fixed_from_int(48));
    next_pointer();
    assert(keys_modifiers() == KEYS_SHIFT);
    keyboard_key(NULL, NULL, 12, 0, KEY_B, WL_KEYBOARD_KEY_STATE_PRESSED);
    keyboard_modifiers(NULL, NULL, 13, 0, 0, 0, 0);
    assert(wl_backend_next_key() == 'B' && keys_modifiers() == KEYS_SHIFT);
    char name[64];
    assert(keys_last_name(name, sizeof name) == 0 && !strcmp(name, "Shift+B"));
    pointer_motion(NULL, NULL, 0, 0, 0);
    next_pointer();
    assert(keys_modifiers() == 0);
    keyboard_modifiers(NULL, NULL, 14, 1u << shift, 0, 0, 0);
    keyboard_leave(NULL, NULL, 15, NULL);
    assert(keys_modifiers() == 0 && wl_backend_keyboard_serial() == 14 && !wl_backend_keyboard_focused());
    puts("PASS pointer events use live modifiers; queued keys keep their modifier snapshot");

    int v = WL_POINTER_AXIS_VERTICAL_SCROLL, h = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    pointer_scroll_events(1);
    pointer_axis_source(NULL, NULL, WL_POINTER_AXIS_SOURCE_FINGER);
    pointer_axis(NULL, NULL, 0, v, wl_fixed_from_int(15));
    pointer_axis(NULL, NULL, 0, h, wl_fixed_from_int(30));
    pointer_frame(NULL, NULL);
    struct pointer_event e = next_pointer();
    assert(e.scroll_phase == SCROLL_BEGIN && e.scroll_source == SCROLL_FINGER);
    e = next_pointer();
    assert(e.wheel == -1 && e.hwheel == 2);
    pointer_axis_stop(NULL, NULL, 0, v);
    pointer_frame(NULL, NULL);
    assert(!pointer_next());
    pointer_axis_stop(NULL, NULL, 0, h);
    pointer_frame(NULL, NULL);
    e = next_pointer();
    assert(e.scroll_phase == SCROLL_END && e.scroll_source == SCROLL_FINGER && !pointer_next());
    pointer_axis_source(NULL, NULL, WL_POINTER_AXIS_SOURCE_FINGER);
    pointer_frame(NULL, NULL); /* source-only frame must not contaminate the next one */
    pointer_axis_value120(NULL, NULL, v, 120);
    pointer_axis_discrete(NULL, NULL, h, 2);
    pointer_frame(NULL, NULL);
    e = next_pointer();
    assert(e.wheel == -1 && e.hwheel == 2 && e.scroll_source == SCROLL_WHEEL && !pointer_next());
    puts("PASS scroll stop retains the finger source; wheel steps are decoded per axis");

    pointer_axis_source(NULL, NULL, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
    pointer_axis(NULL, NULL, 0, v, wl_fixed_from_int(15));
    pointer_frame(NULL, NULL);
    next_pointer();
    next_pointer();
    pointer_button(NULL, NULL, 21, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    pointer_button(NULL, NULL, 22, 0, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    pointer_button(NULL, NULL, 23, 0, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_PRESSED);
    pointer_button(NULL, NULL, 24, 0, BTN_SIDE, WL_POINTER_BUTTON_STATE_PRESSED);
    assert(wl_backend_press_serial() == 23);
    next_pointer();
    next_pointer();
    next_pointer();
    w.enter_serial = 20;
    pointer_axis(NULL, NULL, 0, v, wl_fixed_from_int(15));
    unsigned events = w.events;
    pointer_leave(NULL, NULL, 25, NULL);
    assert(!w.enter_serial && !w.last_axis_source && w.events > events);
    for (int i = 0; i < 3; i++) {
        static const int buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
        e = next_pointer();
        assert(e.button == buttons[i] && !e.pressed && e.cancelled && e.buttons == (7 & ~((2 << i) - 1)));
    }
    e = next_pointer();
    assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_source == SCROLL_CONTINUOUS && !e.buttons);
    e = next_pointer();
    assert(e.x == -1 && e.y == -1 && !e.button && !e.buttons);
    pointer_frame(NULL, NULL);
    assert(!pointer_next());
    puts("PASS pointer leave cancels held buttons and scroll without a genuine release");

    pointer_enter(NULL, NULL, 27, NULL, wl_fixed_from_int(32), wl_fixed_from_int(48));
    next_pointer();
    pointer_button(NULL, NULL, 28, 0, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_RELEASED);
    assert(!pointer_next());
    pointer_button(NULL, NULL, 29, 0, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_PRESSED);
    e = next_pointer();
    assert(e.button == BTN_MIDDLE && e.pressed && !e.cancelled && e.clicks == 1);
    pointer_button(NULL, NULL, 30, 0, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_RELEASED);
    e = next_pointer();
    assert(e.button == BTN_MIDDLE && !e.pressed && !e.cancelled);
    puts("PASS a late release is ignored; the next genuine click starts a new sequence");

    pointer_motion(NULL, NULL, 0, wl_fixed_from_int(32), wl_fixed_from_int(48));
    next_pointer();
    pointer_leave(NULL, NULL, 31, NULL);
    e = next_pointer();
    assert(e.x == -1 && e.y == -1 && !e.button && !e.buttons && !pointer_next());
    puts("PASS pointer leave clears hover without a button or scroll gesture");
    w.display = NULL;
    wl_backend_close();
    return 0;
}
#else
#define COLOUR 0x2A6FDBu

int main(void)
{
    int keep = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    screen_open(BASE_FONT);
    keys_open();
    for (;;) {
        int key = key_get();
        if (key == K_SHOWN) {
            int w, h, gw, gh, top, decoration = 0;
            screen_pixels(&w, &h);
            screen_cells(&gw, &gh, &top);
#ifdef CONFIG_WAYLAND
            decoration = wl_backend_decoration();
#endif
            screen_fill_px(0, 0, w, h, COLOUR);
            screen_bar("window test", NULL);
            screen_text(0, 2, "window backend", 0xFFFFFF, COLOUR);
            screen_flush();
            printf("SHOWN %d %d %d %d %d %d %d\n", w, h, screen_cols(), screen_rows(), top, gh, decoration);
        } else if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->button) printf("BUTTON %d %d %d %d %d\n", e->button, e->pressed, e->clicks, e->x, e->y);
            else if (e->wheel || e->hwheel) printf("WHEEL %d %d\n", e->wheel, e->hwheel);
            else printf("MOTION %d %d\n", e->x, e->y);
        } else if (key == K_CLOSE) {
            printf("CLOSE\n");
            if (keep) {
                key_close_kept();
                printf("KEPT\n");
            }
        } else if (key == K_TEXT) {
            printf("TEXT %s\n", keys_text());
        } else if (key != 0) {
            printf("KEY %d %d\n", key, keys_modifiers());
            if (key == 'k') keep = 1;
            if (key == 'u') keep = 0;
            if (key == 'q') return 0;
        }
    }
}
#endif
