/* Standard wl_keyboard + XKB client (no text-input protocol) for IME verification. */
#define main window_client_main
#include "test-window.c"
#undef main
#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <xkbcommon/xkbcommon.h>

static struct wl_seat *input_seat;
static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xstate;
static char received[256];
static const char *output_file;
static int configures;

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                       int32_t fd, uint32_t size)
{
    (void)data; (void)k;
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0);
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    assert(map != MAP_FAILED);
    struct xkb_keymap *next_map = xkb_keymap_new_from_string(
        xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    assert(next_map);
    struct xkb_state *next_state = xkb_state_new(next_map);
    assert(next_state);
    xkb_state_unref(xstate);
    xkb_keymap_unref(keymap);
    keymap = next_map;
    xstate = next_state;
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys)
{
    (void)data; (void)k; (void)serial; (void)s; (void)keys;
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s)
{
    (void)data; (void)k; (void)serial; (void)s;
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t code, uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xstate) {
        return;
    }
    char utf8[32] = "";
    int n = xkb_state_key_get_utf8(xstate, code + 8, utf8, sizeof utf8);
    if (n > 0 && (unsigned char)utf8[0] >= 32) {
        size_t cur = strlen(received);
        snprintf(received + cur, sizeof received - cur, "%s", utf8);
        if (output_file) {
            FILE *out = fopen(output_file, "w");
            if (out) {
                fprintf(out, "%s\n", received);
                fclose(out);
            }
            /* Which key each character came on: FILE.keys, "CODE CHARACTER". */
            char keys_file[512];
            snprintf(keys_file, sizeof keys_file, "%s.keys", output_file);
            out = fopen(keys_file, "a");
            if (out) {
                fprintf(out, "%u %s\n", code, utf8);
                fclose(out);
            }
        }
    }
}

static void kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t locked, uint32_t group)
{
    (void)data; (void)k; (void)serial;
    if (xstate) {
        xkb_state_update_mask(xstate, dep, lat, locked, 0, 0, group);
    }
}

static void kbd_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)data; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &kbd_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

static void input_global(void *data, struct wl_registry *reg, uint32_t name,
                         const char *interface, uint32_t version)
{
    global(data, reg, name, interface, version);
    if (strcmp(interface, "wl_seat") == 0) {
        input_seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                      version < 5 ? version : 5);
        wl_seat_add_listener(input_seat, &seat_listener, NULL);
    }
}

static void counted_configure(void *data, struct xdg_toplevel *top, int32_t width,
                              int32_t height, struct wl_array *states)
{
    configures++;
    top_configure(data, top, width, height, states);
}

/* test-ime-client FILE: a window that writes the text it receives to FILE
 * (and how often it was reconfigured: the overlay must not resize it). */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test-ime-client FILE\n");
        return 2;
    }
    output_file = argv[1];
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(xkb);
    struct wl_display *display = wl_display_connect(NULL);
    assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener registry_listener = { input_global, removed };
    wl_registry_add_listener(registry, &registry_listener, NULL);
    assert(wl_display_roundtrip(display) >= 0);
    assert(wl_display_roundtrip(display) >= 0);
    assert(input_seat && keyboard && compositor && shm && wm);

    const struct xdg_wm_base_listener wm_listener = { ping };
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    int fd = memfd_create("locachy-kbd-client", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 160 * 100 * 4) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 160 * 100 * 4);
    buffer = wl_shm_pool_create_buffer(pool, 0, 160, 100, 160 * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
    const struct xdg_surface_listener surface_listener = { configure };
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    const struct xdg_toplevel_listener top_listener = { counted_configure, top_close };
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_title(toplevel, "LOCachy standard wl_keyboard test");
    wl_surface_commit(surface);
    for (int i = 0; i < 4; i++) assert(wl_display_roundtrip(display) >= 0);
    int before = configures;
    while (wl_display_dispatch(display) >= 0) {
        if (configures != before) {
            FILE *out = fopen(output_file, "a");
            if (out) {
                fprintf(out, "reconfigured\n");
                fclose(out);
            }
            before = configures;
        }
    }
    return 0;
}
