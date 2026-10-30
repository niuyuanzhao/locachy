/* A resizable xdg-shell client for test-snapping.sh. Default: standard
 * server-side decorations. --client-decorated requests a move itself from
 * its top strip, as a client-decorated application does. Geometry, tiled
 * states and committed sizes are logged; the blue client area is measured
 * independently from compositor screenshots. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct zxdg_decoration_manager_v1 *decorations;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static int width = 320, height = 200, client_decorated;
static double pointer_y;

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static void configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg, serial);
    size_t bytes = (size_t)width * height * 4;
    int fd = memfd_create("snap-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) exit(4);
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) exit(4);
    for (size_t i = 0; i < bytes / 4; i++) pixels[i] = 0xff4488cc;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
    struct wl_buffer *next = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
                                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(pixels, bytes);
    wl_surface_attach(surface, next, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    if (buffer) wl_buffer_destroy(buffer);
    buffer = next;
    printf("size %d %d\n", width, height);
    fflush(stdout);
}

static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
                          struct wl_array *states)
{
    (void)data;
    (void)top;
    if (w > 0) width = w;
    if (h > 0) height = h;
    uint32_t *state;
    printf("states");
    wl_array_for_each(state, states) printf(" %u", *state);
    putchar('\n');
}

static void close_window(void *data, struct xdg_toplevel *top)
{
    (void)data;
    (void)top;
    exit(0);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)serial; (void)entered; (void)x;
    pointer_y = wl_fixed_to_double(y);
    printf("pointer %.2f %.2f\n", wl_fixed_to_double(x), pointer_y);
    fflush(stdout);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *left)
{
    (void)data; (void)pointer; (void)serial; (void)left;
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)time; (void)x;
    pointer_y = wl_fixed_to_double(y);
    printf("pointer %.2f %.2f\n", wl_fixed_to_double(x), pointer_y);
    fflush(stdout);
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)data; (void)pointer; (void)time;
    if (client_decorated && pointer_y < 28 && button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        xdg_toplevel_move(toplevel, seat, serial);
    }
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};
static void capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data;
    static struct wl_pointer *pointer;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static const struct wl_seat_listener seat_listener = { .capabilities = capabilities };

static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode)
{
    (void)data; (void)decoration;
    printf("decoration %u\n", mode);
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = { decoration_configure };

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version)
{
    (void)data; (void)version;
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 3);
    } else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
        decorations = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}
static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

int main(int argc, char **argv)
{
    client_decorated = argc == 2 && strcmp(argv[1], "--client-decorated") == 0;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    const struct wl_registry_listener registry_listener = { global, removed };
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm || !decorations || !seat) return 2;
    const struct xdg_wm_base_listener wm_listener = { ping };
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm, surface);
    const struct xdg_surface_listener surface_listener = { configure };
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg);
    const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = close_window };
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    struct zxdg_toplevel_decoration_v1 *dec =
        zxdg_decoration_manager_v1_get_toplevel_decoration(decorations, toplevel);
    zxdg_toplevel_decoration_v1_add_listener(dec, &decoration_listener, NULL);
    zxdg_toplevel_decoration_v1_set_mode(dec, client_decorated ?
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    xdg_toplevel_set_title(toplevel, "LOCachy edge snapping test");
    xdg_toplevel_set_app_id(toplevel, "locachy-snap-test");
    wl_surface_commit(surface);
    while (wl_display_dispatch(display) >= 0) {}
    return 0;
}
