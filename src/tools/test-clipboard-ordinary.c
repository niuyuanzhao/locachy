/* Ordinary wl_data_device copy/paste through the restricted container
 * display. The test compositor provides synthetic keyboard focus; this
 * client cannot bind data-control or read the host clipboard socket.
 * source publishes on a real key serial; read receives after keyboard focus.
 */
#define _GNU_SOURCE
#define main window_test_main
#include "test-window.c"
#undef main
#include <poll.h>
#include <stdlib.h>
#include <signal.h>
#include "primary-selection-unstable-v1-client-protocol.h"

static struct wl_seat *clipboard_seat;
static struct wl_keyboard *keyboard;
static struct wl_data_device_manager *data_manager;
static struct wl_data_device *data_device;
static struct wl_data_offer *data_offer;
static struct zwp_primary_selection_device_manager_v1 *primary_manager;
static struct zwp_primary_selection_device_v1 *primary_device;
static int received, source_mode, published, forbidden;
static uint32_t copy_serial;
static volatile sig_atomic_t repeat_copy;
static const char *chosen_label;
static void repeat_signal(int sig) { (void)sig; repeat_copy = 1; }
static const char payload[] = "restricted ordinary 中文 🌍\n";

static void data_send(void *data, struct wl_data_source *s, const char *mime, int fd)
{
    (void)data; (void)s; (void)mime;
    const char *delay = getenv("CLIPBOARD_TEST_DELAY_US");
    if (delay) usleep((useconds_t)strtoul(delay, NULL, 10));
    const char *text = chosen_label ? chosen_label : payload;
    size_t size = strlen(text), off = 0;
    while (off < size) {
        ssize_t n = write(fd, text + off, size - off);
        if (n > 0) off += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
}
static void data_cancel(void *data, struct wl_data_source *s)
{ (void)data; puts("ordinary copy cancelled"); fflush(stdout); wl_data_source_destroy(s); }
static void data_target(void *d, struct wl_data_source *s, const char *mime) { (void)d; (void)s; (void)mime; }
static const struct wl_data_source_listener data_source_listener = {
    .target = data_target, .send = data_send, .cancelled = data_cancel,
};
static void primary_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *mime, int fd)
{ (void)s; data_send(d, NULL, mime, fd); }
static void primary_cancel(void *d, struct zwp_primary_selection_source_v1 *s)
{ (void)d; zwp_primary_selection_source_v1_destroy(s); }
static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = primary_send, .cancelled = primary_cancel,
};
static void publish_both(uint32_t serial)
{
    struct wl_data_source *s = wl_data_device_manager_create_data_source(data_manager);
    wl_data_source_add_listener(s, &data_source_listener, NULL);
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_device_set_selection(data_device, s, serial);
    if (primary_device) {
        struct zwp_primary_selection_source_v1 *p = zwp_primary_selection_device_manager_v1_create_source(primary_manager);
        zwp_primary_selection_source_v1_add_listener(p, &primary_source_listener, NULL);
        zwp_primary_selection_source_v1_offer(p, "text/plain;charset=utf-8");
        zwp_primary_selection_device_v1_set_selection(primary_device, p, serial);
    }
}
static void offer_type(void *d, struct wl_data_offer *o, const char *mime) { (void)d; (void)o; (void)mime; }
static const struct wl_data_offer_listener data_offer_listener = { .offer = offer_type };
static void data_new_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{ (void)d; (void)dd; wl_data_offer_add_listener(o, &data_offer_listener, NULL); }
static void data_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
    (void)d; (void)dd;
    if (data_offer) wl_data_offer_destroy(data_offer);
    data_offer = o;
    if (!source_mode && o) {
        int p[2];
        if (pipe(p) < 0) exit(1);
        wl_data_offer_receive(o, "text/plain;charset=utf-8", p[1]);
        close(p[1]);
        wl_display_flush(wl_data_device_get_user_data(dd));
        struct pollfd wait = { .fd = p[0], .events = POLLIN };
        char data[1024];
        size_t size = 0;
        for (;;) {
            if (poll(&wait, 1, 3000) <= 0) exit(1);
            ssize_t n = read(p[0], data + size, sizeof data - size);
            if (n > 0) size += (size_t)n;
            else if (n == 0) break;
            else if (errno != EINTR) exit(1);
        }
        close(p[0]);
        if (size != sizeof payload - 1 || memcmp(data, payload, size)) exit(1);
        puts("ordinary paste received");
        received = 1;
    }
}
static void drag_enter(void *d, struct wl_data_device *dd, uint32_t serial, struct wl_surface *s,
                       wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{ (void)d; (void)dd; (void)serial; (void)s; (void)x; (void)y; (void)o; }
static void drag_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void drag_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void drag_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static const struct wl_data_device_listener data_listener = {
    .data_offer = data_new_offer, .enter = drag_enter, .leave = drag_leave,
    .motion = drag_motion, .drop = drag_drop, .selection = data_selection,
};
static void keymap(void *d, struct wl_keyboard *k, uint32_t format, int fd, uint32_t size)
{ (void)d; (void)k; (void)format; (void)size; close(fd); }
static void enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{ (void)d; (void)k; (void)serial; (void)s; (void)keys; }
static void leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)k; (void)serial; (void)s; }
static void key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t code, uint32_t state)
{
    (void)d; (void)k; (void)t; (void)code;
    if (source_mode && state) {
        copy_serial = serial;
        puts("serial received"); fflush(stdout);
        if (published) return;
        publish_both(serial);
        published = 1;
        puts("ordinary copy published"); fflush(stdout);
    }
}
static void mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g)
{ (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keymap, .enter = enter, .leave = leave, .key = key, .modifiers = mods,
};
static void clip_global(void *d, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
    global(d, r, name, interface, version);
    if (strcmp(interface, wl_seat_interface.name) == 0) clipboard_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) data_manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, 1);
    else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0)
        primary_manager = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    else if (strstr(interface, "data_control_manager")) forbidden = 1;
}

int main(int argc, char **argv)
{
    source_mode = argc >= 2 && strcmp(argv[1], "source") == 0;
    chosen_label = argc >= 3 ? argv[2] : NULL;
    signal(SIGUSR1, repeat_signal);
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *r = wl_display_get_registry(display);
    const struct wl_registry_listener listener = { clip_global, removed };
    wl_registry_add_listener(r, &listener, NULL);
    wl_display_roundtrip(display);
    if (forbidden || !clipboard_seat || !data_manager || !compositor || !shm || !wm) return 2;
    keyboard = wl_seat_get_keyboard(clipboard_seat);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    data_device = wl_data_device_manager_get_data_device(data_manager, clipboard_seat);
    wl_data_device_add_listener(data_device, &data_listener, display);
    if (primary_manager && source_mode) primary_device = zwp_primary_selection_device_manager_v1_get_device(primary_manager, clipboard_seat);
    static const struct xdg_wm_base_listener wm_listener = { .ping = ping };
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    int fd = memfd_create("ordinary-clipboard-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 160 * 100 * 4) < 0) return 1;
    uint32_t *pixels = mmap(NULL, 160 * 100 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) return 1;
    for (int i = 0; i < 160 * 100; i++) pixels[i] = 0xff4488cc;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 160 * 100 * 4);
    buffer = wl_shm_pool_create_buffer(pool, 0, 160, 100, 160 * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); close(fd);
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
    static const struct xdg_surface_listener surface_listener = { .configure = configure };
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    static const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "clipboard-ordinary");
    xdg_toplevel_set_title(toplevel, source_mode ? "Clipboard source" : "Clipboard target");
    wl_surface_commit(surface);
    while (!received) {
        if (repeat_copy && copy_serial) {
            repeat_copy = 0;
            publish_both(copy_serial);
            puts("ordinary copy repeated"); fflush(stdout);
        }
        if (wl_display_dispatch_pending(display) < 0 || wl_display_flush(display) < 0) break;
        struct pollfd p = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&p, 1, 50) > 0 && wl_display_dispatch(display) < 0) break;
    }
    return received ? 0 : 1;
}
