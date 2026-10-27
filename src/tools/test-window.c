/* A C Wayland test client that can minimize its only window: test-window
 * [--minimize | --title TITLE [APP_ID]]. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static int configured;

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static void configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg, serial);
    if (!configured) {
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, 160, 100);
        wl_surface_commit(surface);
        configured = 1;
    }
}

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    }
}

static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

/* Prints "activated" each time the compositor gives it focus. */
static void top_configure(void *data, struct xdg_toplevel *top, int32_t width,
                          int32_t height, struct wl_array *states)
{
    static int was_active;
    uint32_t *state;
    int active = 0;
    (void)data;
    (void)top;
    (void)width;
    (void)height;
    wl_array_for_each(state, states) {
        active |= *state == XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    if (active && !was_active) {
        printf("activated\n");
        fflush(stdout);
    }
    was_active = active;
}

/* Prints "close" when asked to close (and stays). */
static void top_close(void *data, struct xdg_toplevel *top)
{
    (void)data;
    (void)top;
    printf("close\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener registry_listener = { global, removed };
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm) {
        return 2;
    }
    const struct xdg_wm_base_listener wm_listener = { ping };
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    int fd = memfd_create("locachy-test-window", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 160 * 100 * 4) != 0) {
        return 3;
    }
    uint32_t *pixels = mmap(NULL, 160 * 100 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        return 3;
    }
    for (int i = 0; i < 160 * 100; i++) {
        pixels[i] = 0xff4488cc;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 160 * 100 * 4);
    buffer = wl_shm_pool_create_buffer(pool, 0, 160, 100, 160 * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
    const struct xdg_surface_listener surface_listener = { configure };
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    const char *title = argc > 2 && strcmp(argv[1], "--title") == 0 ? argv[2] : "LOCachy lifecycle test";
    xdg_toplevel_set_title(toplevel, title);
    xdg_toplevel_set_app_id(toplevel, argc > 3 && strcmp(argv[1], "--title") == 0 ? argv[3] :
                                      "locachy-test-window");
    wl_surface_commit(surface);
    for (int i = 0; i < 4; i++) {
        if (wl_display_roundtrip(display) < 0) {
            return 4;
        }
    }
    if (argc > 1 && strcmp(argv[1], "--minimize") == 0) {
        xdg_toplevel_set_minimized(toplevel);
        wl_display_roundtrip(display);
    }
    while (wl_display_dispatch(display) >= 0) {
    }
    return 0;
}
