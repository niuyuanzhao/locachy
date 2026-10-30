/* Real pointer drag in a disposable compositor, no uinput:
 * test-snap-drag X0 Y0 X1 Y1 LAYOUT_WIDTH LAYOUT_HEIGHT. Coordinates are
 * relative to the layout bounding box (including negative-position outputs).
 * One virtual pointer remains alive from press through release. */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version)
{
    (void)data; (void)version;
    if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, "zwlr_virtual_pointer_manager_v1") == 0) {
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}
static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static uint32_t now_ms(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint32_t)(time.tv_sec * 1000 + time.tv_nsec / 1000000);
}
int main(int argc, char **argv)
{
    int stream = argc == 6 && strcmp(argv[1], "--stream") == 0;
    if (!stream && argc != 7) {
        fprintf(stderr, "usage: test-snap-drag X0 Y0 X1 Y1 LAYOUT_WIDTH LAYOUT_HEIGHT\n");
        return 2;
    }
    int x = atoi(argv[stream ? 2 : 1]), y = atoi(argv[stream ? 3 : 2]);
    int tx = stream ? 0 : atoi(argv[3]), ty = stream ? 0 : atoi(argv[4]);
    int width = atoi(argv[stream ? 4 : 5]), height = atoi(argv[stream ? 5 : 6]);
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    const struct wl_registry_listener listener = { global, removed };
    wl_registry_add_listener(wl_display_get_registry(display), &listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !manager) return 1;
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), x, y, width, height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(150000);
    if (stream) {
        char command[64];
        double dx, dy;
        int steps, delay;
        while (fgets(command, sizeof command, stdin)) {
            if (strncmp(command, "press", 5) == 0 || strncmp(command, "release", 7) == 0) {
                zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT, command[0] == 'p' ?
                    WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                zwlr_virtual_pointer_v1_frame(pointer);
                wl_display_roundtrip(display);
            } else if (sscanf(command, "push %lf %lf %d %d", &dx, &dy, &steps, &delay) == 4) {
                for (int i = 0; i < steps; i++) {
                    zwlr_virtual_pointer_v1_motion(pointer, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
                    zwlr_virtual_pointer_v1_frame(pointer);
                    wl_display_roundtrip(display);
                    usleep((useconds_t)delay * 1000);
                }
            } else if (sscanf(command, "warp %lf %lf", &dx, &dy) == 2) {
                zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)dx, (uint32_t)dy, width, height);
                zwlr_virtual_pointer_v1_frame(pointer);
                wl_display_roundtrip(display);
            } else if (sscanf(command, "wait %d", &delay) == 1) {
                usleep((useconds_t)delay * 1000);
            } else if (strncmp(command, "quit", 4) == 0) {
                break;
            }
            puts("done");
            fflush(stdout);
        }
        zwlr_virtual_pointer_v1_destroy(pointer);
        wl_display_roundtrip(display);
        return 0;
    }
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    for (int i = 1; i <= 40; i++) {
        zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), x + (tx - x) * i / 40,
                                               y + (ty - y) * i / 40, width, height);
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_roundtrip(display);
        usleep(20000);
    }
    /* An exact-target geometry check may deliberately cross a held inner
     * edge. Wait out its hold, then move to the requested final coordinate.
     * Continuous hold behavior is tested separately by --stream. */
    usleep(800000);
    zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), tx, ty, width, height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(550000); /* stock snapping preview delay, then release */
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(250000);
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_roundtrip(display);
    return 0;
}
