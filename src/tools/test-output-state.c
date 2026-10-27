/* Read back compositor output state without requesting a configuration.
 * Reuse event decoders, not the controller's configuration callbacks. */
#define main controller_main
#include "../progs/labwc-control.c"
#undef main

static int got_state;

static void state_done(void *data, struct zwlr_output_manager_v1 *manager, uint32_t serial)
{
    (void)data;
    (void)manager;
    (void)serial;
    got_state = 1;
}

static const struct zwlr_output_manager_v1_listener state_listener = {
    .head = new_head,
    .done = state_done,
    .finished = outputs_finished,
};

static void state_global(void *data, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version)
{
    (void)data;
    if (!outputs && strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        output_version = version < 4 ? version : 4;
        outputs = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, output_version);
        zwlr_output_manager_v1_add_listener(outputs, &state_listener, NULL);
    }
}

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = { state_global, global_remove };
    wl_registry_add_listener(registry, &listener, NULL);
    for (int i = 0; i < 3; i++) {
        if (wl_display_roundtrip(display) < 0) return 1;
    }
    if (!outputs || !got_state) return 1;
    for (struct head *h = heads; h; h = h->next) {
        printf("%s %d %d %d %d %d %.6f\n", h->name, h->enabled, h->x, h->y,
               h->current ? h->current->width : 0, h->current ? h->current->height : 0,
               h->scale);
    }
    wl_display_disconnect(display);
    return 0;
}
