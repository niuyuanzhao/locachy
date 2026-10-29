/* Focused standard Wayland client: record actual keyboard and pointer events. */
#define main window_client_main
#include "test-window.c"
#undef main
#include <assert.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct xkb_context *context;
static struct xkb_state *state;
static FILE *logfile;

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kb;
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0);
    char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    assert(text != MAP_FAILED);
    struct xkb_keymap *map = xkb_keymap_new_from_string(context, text, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(text, size);
    assert(map);
    xkb_state_unref(state);
    state = xkb_state_new(map);
    xkb_keymap_unref(map);
    assert(state);
    fprintf(logfile, "MAP\n");
    fflush(logfile);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surf, struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial; (void)surf; (void)keys;
    fprintf(logfile, "FOCUS\n");
    fflush(logfile);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surf)
{
    (void)data; (void)kb; (void)serial; (void)surf;
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                         uint32_t code, uint32_t value)
{
    (void)data; (void)kb; (void)serial; (void)time;
    char text[64] = "";
    if (value && state) xkb_state_key_get_utf8(state, code + 8, text, sizeof text);
    fprintf(logfile, "KEY %u %u %s\n", code, value, text);
    fflush(logfile);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)kb; (void)serial;
    if (state) xkb_state_update_mask(state, depressed, latched, locked, 0, 0, group);
    fprintf(logfile, "MOD %u %u %u %u\n", depressed, latched, locked, group);
    fflush(logfile);
}

static void keyboard_repeat(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)serial; (void)surf; (void)x; (void)y;
    fprintf(logfile, "POINTER\n");
    fflush(logfile);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf)
{
    (void)data; (void)p; (void)serial; (void)surf;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t value)
{
    (void)data; (void)p; (void)serial; (void)time;
    fprintf(logfile, "BUTTON %u %u\n", button, value);
    fflush(logfile);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)p; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void client_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg, serial);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
}

static void client_size(void *data, struct xdg_toplevel *top, int32_t width, int32_t height, struct wl_array *states)
{
    top_configure(data, top, width, height, states);
    if (width <= 0 || height <= 0) return;
    int fd = memfd_create("input-delivery-resize", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, (off_t)width * height * 4) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, width * height * 4);
    struct wl_buffer *next = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    close(fd);
    wl_shm_pool_destroy(pool);
    if (buffer) wl_buffer_destroy(buffer);
    buffer = next;
}

static void input_global(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version)
{
    global(data, reg, name, interface, version);
    if (!strcmp(interface, "wl_seat")) {
        struct wl_seat *seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    logfile = fopen(argv[1], "w");
    assert(logfile);
    context = xkb_context_new(0);
    assert(context);
    struct wl_display *dpy = wl_display_connect(NULL);
    assert(dpy);
    struct wl_registry *reg = wl_display_get_registry(dpy);
    static const struct wl_registry_listener reg_listener = { .global = input_global, .global_remove = removed };
    wl_registry_add_listener(reg, &reg_listener, NULL);
    assert(wl_display_roundtrip(dpy) >= 0 && wl_display_roundtrip(dpy) >= 0);
    assert(compositor && shm && wm);
    static const struct xdg_wm_base_listener wm_listener = { .ping = ping };
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    int fd = memfd_create("input-delivery-client", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 160 * 100 * 4) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 160 * 100 * 4);
    buffer = wl_shm_pool_create_buffer(pool, 0, 160, 100, 160 * 4, WL_SHM_FORMAT_ARGB8888);
    close(fd);
    wl_shm_pool_destroy(pool);
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
    static const struct xdg_surface_listener surface_listener = { .configure = client_configure };
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    static const struct xdg_toplevel_listener top_listener = { .configure = client_size, .close = top_close };
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Input delivery acceptance");
    xdg_toplevel_set_maximized(toplevel);
    wl_surface_commit(surface);
    while (wl_display_dispatch(dpy) >= 0) {}
    return 0;
}
