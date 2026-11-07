/*
 * test-drop-client: one side of a drag-and-drop in a disposable compositor.
 *   test-drop-client source APP_ID MIME...  a 400x300 window that starts a
 *                                           copy drag offering MIME on its
 *                                           first button press
 *   test-drop-client target APP_ID          a window that accepts
 *                                           text/uri-list and reads it on drop
 *   test-drop-client ping COUNT             COUNT roundtrips, then exits
 *   test-drop-client escape                 presses and releases Escape on
 *                                           a virtual keyboard, then exits
 * Every event is a line on standard output after a CLOCK_MONOTONIC time in
 * milliseconds: "pid N", "ready", "drag-start", "send MIME", "cancelled",
 * "dropped", "finished", "enter", "leave", "drop", "data TEXT",
 * "button N pressed|released", "key N pressed|released", "ping COUNT MS",
 * "escape".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define WIDTH 400
#define HEIGHT 300
#define URI_LIST "text/uri-list"
#define PAYLOAD "file:///run/user/1000/doc/a1b2c3/report.txt\r\n"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_data_device_manager *manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboards;
static struct wl_data_device *device;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static struct wl_data_source *source;
static struct wl_data_offer *offer;
static int is_source, offer_has_uri_list, configured;
static char **mime_types;
static int mime_count;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list args;
    printf("%.3f ", now_ms());
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static void source_target(void *data, struct wl_data_source *s, const char *mime)
{
    (void)data; (void)s; (void)mime;
}

static void source_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd)
{
    (void)data; (void)s;
    say("send %s", mime);
    const char *text = strcmp(mime, URI_LIST) == 0 ? PAYLOAD : "LOCACHYDOCkey";
    if (write(fd, text, strlen(text)) < 0) say("write failed: %s", strerror(errno));
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s)
{
    (void)data;
    say("cancelled");
    wl_data_source_destroy(s);
    source = NULL;
}

static void source_dropped(void *data, struct wl_data_source *s)
{
    (void)data; (void)s;
    say("dropped");
}

static void source_finished(void *data, struct wl_data_source *s)
{
    (void)data;
    say("finished");
    wl_data_source_destroy(s);
    source = NULL;
}

static void source_action(void *data, struct wl_data_source *s, uint32_t action)
{
    (void)data; (void)s; (void)action;
}

static const struct wl_data_source_listener source_listener = {
    source_target, source_send, source_cancelled, source_dropped, source_finished, source_action,
};

static void offer_offer(void *data, struct wl_data_offer *o, const char *mime)
{
    (void)data; (void)o;
    if (strcmp(mime, URI_LIST) == 0) offer_has_uri_list = 1;
}

static void offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions)
{
    (void)data; (void)o; (void)actions;
}

static void offer_action(void *data, struct wl_data_offer *o, uint32_t action)
{
    (void)data; (void)o; (void)action;
}

static const struct wl_data_offer_listener offer_listener = {
    offer_offer, offer_source_actions, offer_action,
};

static void device_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
    (void)data; (void)d;
    offer_has_uri_list = 0;
    wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void device_enter(void *data, struct wl_data_device *d, uint32_t serial, struct wl_surface *s,
                         wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
    (void)data; (void)d; (void)s; (void)x; (void)y;
    if (is_source) return;
    say("enter");
    offer = o;
    if (o && offer_has_uri_list) {
        wl_data_offer_accept(o, serial, URI_LIST);
        wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
}

static void device_leave(void *data, struct wl_data_device *d)
{
    (void)data; (void)d;
    if (is_source) return;
    say("leave");
    if (offer) wl_data_offer_destroy(offer);
    offer = NULL;
}

static void device_motion(void *data, struct wl_data_device *d, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)d; (void)time; (void)x; (void)y;
}

static void device_drop(void *data, struct wl_data_device *d)
{
    (void)data; (void)d;
    say("drop");
    if (!offer) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;
    wl_data_offer_receive(offer, URI_LIST, p[1]);
    close(p[1]);
    wl_display_flush(wl_proxy_get_display((struct wl_proxy *)offer));
    char text[512];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof text - 1 && (r = read(p[0], text + n, sizeof text - 1 - n)) > 0) n += (size_t)r;
    close(p[0]);
    text[n] = 0;
    text[strcspn(text, "\r\n")] = 0;
    say("data %s", text);
    wl_data_offer_finish(offer);
    wl_data_offer_destroy(offer);
    offer = NULL;
}

static void device_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
    (void)data; (void)d;
    if (o && o != offer) wl_data_offer_destroy(o);
}

static const struct wl_data_device_listener device_listener = {
    device_data_offer, device_enter, device_leave, device_motion, device_drop, device_selection,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)serial; (void)s; (void)x; (void)y;
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)data; (void)p; (void)serial; (void)s;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)data; (void)p; (void)time;
    if (!is_source) say("button %u %s", button, state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");
    if (!is_source || state != WL_POINTER_BUTTON_STATE_PRESSED || source) return;
    source = wl_data_device_manager_create_data_source(manager);
    wl_data_source_add_listener(source, &source_listener, NULL);
    for (int i = 0; i < mime_count; i++) wl_data_source_offer(source, mime_types[i]);
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(device, source, surface, NULL, serial);
    say("drag-start");
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)p; (void)time; (void)axis; (void)value;
}

/* Version 1 objects: the later events never come. */
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)k; (void)format; (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                           struct wl_array *keys)
{
    (void)data; (void)k; (void)serial; (void)s; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)data; (void)k; (void)serial; (void)s;
}

static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)time;
    say("key %u %s", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)k; (void)serial; (void)depressed; (void)latched; (void)locked; (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t capabilities)
{
    static struct wl_pointer *pointer;
    static struct wl_keyboard *keyboard;
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    if (!is_source && (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities };

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xdg_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(x, serial);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    configured = 1;
}

static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{
    (void)data; (void)t; (void)w; (void)h; (void)s;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)data; (void)t;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version)
{
    (void)data; (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        keyboards = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static struct wl_buffer *make_buffer(uint32_t colour)
{
    int size = WIDTH * HEIGHT * 4;
    int fd = memfd_create("test-drop-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) return NULL;
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) return NULL;
    for (int i = 0; i < WIDTH * HEIGHT; i++) pixels[i] = colour;
    munmap(pixels, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/* A virtual keyboard with the default keymap: Escape down and up. */
static int press_escape(struct wl_display *display)
{
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (!seat || !keyboards || !text) {
        fprintf(stderr, "test-drop-client: no virtual keyboard\n");
        return 1;
    }
    size_t size = strlen(text) + 1;
    int fd = memfd_create("test-drop-client", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) return 1;
    struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboards, seat);
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    wl_display_roundtrip(display);
    zwp_virtual_keyboard_v1_key(keyboard, 0, KEY_ESC, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_display_roundtrip(display);
    zwp_virtual_keyboard_v1_key(keyboard, 1, KEY_ESC, WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_roundtrip(display);
    say("escape");
    zwp_virtual_keyboard_v1_destroy(keyboard);
    wl_display_roundtrip(display);
    return 0;
}

int main(int argc, char **argv)
{
    int escape = argc == 2 && strcmp(argv[1], "escape") == 0;
    if (!escape && (argc < 3 || (strcmp(argv[1], "source") && strcmp(argv[1], "target") &&
                                 strcmp(argv[1], "ping")))) {
        fprintf(stderr, "usage: test-drop-client source APP_ID MIME... | target APP_ID | ping COUNT | escape\n");
        return 2;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "test-drop-client: no display\n");
        return 1;
    }
    if (strcmp(argv[1], "ping") == 0) {
        int count = atoi(argv[2]);
        double start = now_ms();
        for (int i = 0; i < count; i++) {
            if (wl_display_roundtrip(display) < 0) return 1;
        }
        say("ping %d %.3f", count, now_ms() - start);
        return 0;
    }
    is_source = strcmp(argv[1], "source") == 0;
    mime_types = argv + 3;
    mime_count = argc - 3;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (escape) return press_escape(display);
    if (!compositor || !shm || !wm_base || !seat || !manager) {
        fprintf(stderr, "test-drop-client: missing globals\n");
        return 1;
    }
    say("pid %ld", (long)getpid());
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);
    device = wl_data_device_manager_get_data_device(manager, seat);
    wl_data_device_add_listener(device, &device_listener, NULL);
    buffer = make_buffer(is_source ? 0xff3060a0 : 0xff40a040);
    if (!buffer) return 1;
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, argv[2]);
    xdg_toplevel_set_title(toplevel, argv[2]);
    wl_surface_commit(surface);
    while (!configured && wl_display_dispatch(display) >= 0) { }
    wl_display_roundtrip(display);
    say("ready");
    while (wl_display_dispatch(display) >= 0) { }
    return 0;
}
