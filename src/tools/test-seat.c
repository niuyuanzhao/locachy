/*
 * test-seat: a keyboard (zwp-virtual-keyboard-v1, a us keymap) and a pointer
 * (wlr-virtual-pointer) for a headless labwc, there until it is killed, so
 * that a program that asks the seat for them (Qt's: WeChat) finds them as on
 * the real screen. Nothing is pressed or moved unless the isolated test
 * sends SIGUSR1, which presses and releases C (a real selection serial).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *keyboards;
static struct zwlr_virtual_pointer_manager_v1 *pointers;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface,
                   uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        keyboards = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        pointers = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static volatile sig_atomic_t press;
static void on_press(int sig) { (void)sig; press = 1; }

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "test-seat: no Wayland display\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !keyboards || !pointers) {
        fprintf(stderr, "test-seat: no seat, or no virtual keyboards or pointers here\n");
        return 1;
    }
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (!text) {
        fprintf(stderr, "test-seat: no keymap\n");
        return 1;
    }
    size_t size = strlen(text) + 1;
    int fd = memfd_create("test-seat", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
        fprintf(stderr, "test-seat: cannot hand over the keymap\n");
        return 1;
    }
    struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboards, seat);
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointers, seat);
    wl_display_roundtrip(display);
    signal(SIGUSR1, on_press);
    for (;;) {
        if (press) {
            press = 0;
            zwp_virtual_keyboard_v1_key(keyboard, 0, 46, WL_KEYBOARD_KEY_STATE_PRESSED);
            zwp_virtual_keyboard_v1_key(keyboard, 1, 46, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if (wl_display_dispatch_pending(display) < 0 || wl_display_flush(display) < 0) break;
        struct pollfd p = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&p, 1, 100) > 0 && wl_display_dispatch(display) < 0) break;
    }
    return 0;
}
