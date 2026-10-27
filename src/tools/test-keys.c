/*
 * test-keys KEY...: each key pressed and released, in turn, by a virtual
 * keyboard (zwp-virtual-keyboard-v1, a us keymap), for the tests to type into
 * what the host clients draw on a headless labwc. A KEY is an xkb key name
 * (a, Return, Escape, Left, BackSpace, space...), after Super+, Ctrl+, Alt+
 * or Shift+ to hold those.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *keyboards;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface,
                   uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        keyboards = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

/* The key that gives sym without a modifier; 0 if none. */
static xkb_keycode_t key_of(struct xkb_keymap *keymap, xkb_keysym_t sym)
{
    for (xkb_keycode_t k = xkb_keymap_min_keycode(keymap); k <= xkb_keymap_max_keycode(keymap); k++) {
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(keymap, k, 0, 0, &syms);
        for (int i = 0; i < n; i++) {
            if (syms[i] == sym) return k;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (argc < 2 || !display) {
        fprintf(stderr, "usage: test-keys KEY... (on a Wayland display)\n");
        return 2;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (!seat || !keyboards || !text) {
        fprintf(stderr, "test-keys: no seat, virtual keyboards or keymap\n");
        return 1;
    }
    size_t size = strlen(text) + 1;
    int fd = memfd_create("test-keys", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) return 1;
    struct zwp_virtual_keyboard_v1 *kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboards, seat);
    zwp_virtual_keyboard_v1_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    wl_display_roundtrip(display);
    usleep(200000);
    static const struct { const char *prefix, *mod; } mods[] = {
        { "Super+", XKB_MOD_NAME_LOGO }, { "Ctrl+", XKB_MOD_NAME_CTRL },
        { "Alt+", XKB_MOD_NAME_ALT }, { "Shift+", XKB_MOD_NAME_SHIFT },
    };
    uint32_t t = 1;
    for (int a = 1; a < argc; a++) {
        const char *name = argv[a];
        uint32_t held = 0;
        for (int found = 1; found;) {
            found = 0;
            for (size_t m = 0; m < sizeof mods / sizeof mods[0]; m++) {
                size_t len = strlen(mods[m].prefix);
                if (strncmp(name, mods[m].prefix, len) == 0) {
                    held |= 1u << xkb_keymap_mod_get_index(keymap, mods[m].mod);
                    name += len;
                    found = 1;
                }
            }
        }
        xkb_keycode_t key = key_of(keymap, xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS));
        if (!key) {
            fprintf(stderr, "test-keys: no key %s\n", name);
            return 1;
        }
        zwp_virtual_keyboard_v1_modifiers(kb, held, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb, t++, key - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(kb, t++, key - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
        zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0);
        wl_display_roundtrip(display);
        usleep(150000);
    }
    zwp_virtual_keyboard_v1_destroy(kb);
    wl_display_roundtrip(display);
    return 0;
}
