/*
 * test-click X Y WIDTH HEIGHT [left|middle|move|push MS]: a click at (X, Y) of
 * the screens' layout, WIDTH x HEIGHT, from a virtual pointer
 * (wlr-virtual-pointer): for the tests to click on what the host clients draw
 * on a headless labwc. move: the pointer only goes there; push: then it is
 * moved up and left, every 10 ms for MS ms (into a corner it stays in).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface,
                   uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: test-click X Y WIDTH HEIGHT [left|middle|move|push MS]\n");
        return 2;
    }
    uint32_t x = (uint32_t)atoi(argv[1]), y = (uint32_t)atoi(argv[2]);
    uint32_t w = (uint32_t)atoi(argv[3]), h = (uint32_t)atoi(argv[4]);
    uint32_t button = argc > 5 && strcmp(argv[5], "middle") == 0 ? BTN_MIDDLE : BTN_LEFT;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager) {
        fprintf(stderr, "test-click: no virtual pointer manager\n");
        return 1;
    }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    wl_display_roundtrip(display);
    usleep(300000); /* the clients bind the new pointer */
    zwlr_virtual_pointer_v1_motion_absolute(p, now_ms(), x, y, w, h);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(display);
    usleep(100000);
    if (argc > 5 && strcmp(argv[5], "move") == 0) return 0;
    if (argc > 6 && strcmp(argv[5], "push") == 0) {
        for (int ms = 0; ms < atoi(argv[6]); ms += 10) {
            zwlr_virtual_pointer_v1_motion(p, now_ms(), wl_fixed_from_int(-3), wl_fixed_from_int(-3));
            zwlr_virtual_pointer_v1_frame(p);
            wl_display_flush(display);
            usleep(10000);
        }
        wl_display_roundtrip(display);
        return 0;
    }
    zwlr_virtual_pointer_v1_button(p, now_ms(), button, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(p);
    zwlr_virtual_pointer_v1_button(p, now_ms(), button, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(display);
    return 0;
}
