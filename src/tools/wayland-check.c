/* Inspect a display without changing keyboard focus or owning a surface. */
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)registry;
    (void)name;
    printf("%s %u\n", interface, version);
}

static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        perror("wayland-check: connect");
        return 1;
    }
    const struct wl_registry_listener listener = { global, removed };
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    int result = wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result < 0;
}
