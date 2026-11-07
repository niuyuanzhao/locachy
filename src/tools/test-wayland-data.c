/*
 * test-wayland-data: the real wayland-data.c (built with CONFIG_WAYLAND) in a
 * small window of a headless compositor, driven by lines on stdin. Its own
 * virtual keyboard and pointer give the window real serials; other clients
 * (a data-control helper, foot) are the other side of each transfer.
 *   unit                       write-queue checks, no compositor
 *   (no argument)              commands:
 *   key                        press a key: a keyboard serial of ours
 *   set [primary|png] TEXT     wl_data_clipboard_set (png: hex bytes)
 *   get [primary|png]          wl_data_clipboard_get: status, size, hex bytes
 *   clear [primary]            wl_data_clipboard_clear
 *   where                      our window's content origin, in layout pixels
 *   drag X Y X2 Y2 PATH        press at X,Y, drag PATH's URI to X2,Y2, release
 *   quit
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CONFIG_WAYLAND 1
#include "../base/wayland-data.c"
#include <assert.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static uint32_t key_serial, button_serial;
uint32_t wl_backend_keyboard_serial(void) { return key_serial; }
uint32_t wl_backend_button_serial(void) { return button_serial; }

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vkm;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwp_virtual_keyboard_v1 *vk;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wl_surface *surface;
static struct wl_registry *registry;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_output *output;
static struct wl_buffer *buffer;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *toplevel;
static int configured, focused, keys_seen, presses_seen, entered, out_w = 1920, out_h = 1080;
static double enter_x, enter_y;
static uint32_t ticks;

#define WIN_W 200
#define WIN_H 150

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) { (void)data; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    if (!configured) {
        int stride = WIN_W * 4, size = stride * WIN_H, fd = memfd_create("win", MFD_CLOEXEC);
        assert(fd >= 0 && ftruncate(fd, size) == 0);
        uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(px != MAP_FAILED);
        for (int i = 0; i < WIN_W * WIN_H; i++) px[i] = 0xff336699;
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
        buffer = wl_shm_pool_create_buffer(pool, 0, WIN_W, WIN_H, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        munmap(px, (size_t)size);
        close(fd);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, WIN_W, WIN_H);
    }
    wl_surface_commit(surface);
    configured = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{ (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *a)
{ (void)d; (void)k; (void)a; if (s == surface) { focused = 1; key_serial = serial; } }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)k; (void)serial; if (s == surface) focused = 0; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{ (void)d; (void)k; (void)t; (void)key; (void)state; key_serial = serial; keys_seen++; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g)
{ (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) { (void)d; (void)k; (void)r; (void)delay; }
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

static void pt_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)serial;
    if (s == surface) { entered = 1; enter_x = wl_fixed_to_double(x); enter_y = wl_fixed_to_double(y); }
}
static void pt_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)p; (void)serial; if (s == surface) entered = 0; }
static void pt_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; enter_x = wl_fixed_to_double(x); enter_y = wl_fixed_to_double(y); }
static void pt_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state)
{
    (void)d; (void)p; (void)t; (void)b;
    button_serial = serial;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) presses_seen++;
}
static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void pt_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pt_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pt_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void pt_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener pt_listener = {
    .enter = pt_enter, .leave = pt_leave, .motion = pt_motion, .button = pt_button, .axis = pt_axis,
    .frame = pt_frame, .axis_source = pt_axis_source, .axis_stop = pt_axis_stop, .axis_discrete = pt_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    static int have_kb, have_pt;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !have_kb) {
        have_kb = 1;
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !have_pt) {
        have_pt = 1;
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pt_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                         const char *mk, const char *md, int32_t t)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)t; }
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{ (void)d; (void)o; (void)r; if (flags & WL_OUTPUT_MODE_CURRENT) { out_w = w; out_h = h; } }
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static const struct wl_output_listener out_listener = { .geometry = out_geometry, .mode = out_mode,
                                                        .done = out_done, .scale = out_scale };

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    (void)d;
    wl_data_global(r, name, iface, version);
    if (!strcmp(iface, wl_compositor_interface.name)) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, wl_output_interface.name) && !output) {
        output = wl_registry_bind(r, name, &wl_output_interface, 2);
        wl_output_add_listener(output, &out_listener, NULL);
    }
    else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) vkm = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) vpm = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { global, global_remove };

/* One round of the loop a window backend runs: the display, the data
 * module's pending writes, and stdin when asked. 1: stdin is readable. */
static int pump(int timeout_ms, int with_stdin)
{
    struct pollfd p[2 + WRITES_MAX];
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    wl_display_flush(display);
    size_t n = 0;
    p[n++] = (struct pollfd){ .fd = wl_display_get_fd(display), .events = POLLIN };
    size_t in = n;
    if (with_stdin) p[n++] = (struct pollfd){ .fd = 0, .events = POLLIN };
    size_t first = n;
    int data_n = wl_data_poll(p + n, WRITES_MAX);
    n += (size_t)data_n;
    int t = wl_data_timeout();
    if (t < 0 || (timeout_ms >= 0 && timeout_ms < t)) t = timeout_ms;
    int r = poll(p, n, t);
    if (r > 0 && (p[0].revents & POLLIN)) wl_display_read_events(display);
    else wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
    wl_data_dispatch(p + first, (size_t)data_n);
    return with_stdin && r > 0 && (p[in].revents & (POLLIN | POLLHUP));
}

static int wait_until(int *flag, int want, int ms)
{
    int64_t until = now_ms() + ms;
    while (*flag < want && now_ms() < until) pump(20, 0);
    return *flag >= want;
}

static void settle(int ms)
{
    int64_t until = now_ms() + ms;
    while (now_ms() < until) pump(10, 0);
}

static void pointer_to(int x, int y)
{
    zwlr_virtual_pointer_v1_motion_absolute(vp, ticks += 10, (uint32_t)x, (uint32_t)y, (uint32_t)out_w, (uint32_t)out_h);
    zwlr_virtual_pointer_v1_frame(vp);
    settle(40);
}

static void button(int pressed)
{
    zwlr_virtual_pointer_v1_button(vp, ticks += 10, BTN_LEFT,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(vp);
}

static void print_get(int primary, const char *mime)
{
    int fd;
    size_t size;
    int64_t t0 = now_ms();
    int status = wl_data_clipboard_get(primary, mime, &fd, &size);
    printf("get status=%d size=%zu ms=%lld", status, size, (long long)(now_ms() - t0));
    if (status == CLIPBOARD_OK) {
        int seals = fcntl(fd, F_GET_SEALS);
        unsigned char c;
        printf(" sealed=%d data=", (seals & F_SEAL_WRITE) != 0);
        while (read(fd, &c, 1) == 1) printf("%02x", c);
        close(fd);
    }
    printf("\n");
}

static size_t unhex(char *out, size_t size, const char *in)
{
    size_t n = strlen(in) / 2;
    for (size_t i = 0; i < n && i < size; i++) {
        unsigned v;
        sscanf(in + 2 * i, "%2x", &v);
        out[i] = (char)v;
    }
    return n < size ? n : size;
}

static int command(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    int primary = arg && !strncmp(arg, "primary", 7), png = arg && !strncmp(arg, "png", 3);
    if (!strcmp(line, "quit")) return 0;
    if (!strcmp(line, "key")) {
        int before = keys_seen;
        zwp_virtual_keyboard_v1_key(vk, ticks += 10, KEY_C, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(vk, ticks += 10, KEY_C, WL_KEYBOARD_KEY_STATE_RELEASED);
        int ok = wait_until(&keys_seen, before + 2, 2000);
        printf("key ok=%d focused=%d serial=%u\n", ok, focused, key_serial);
    } else if (!strcmp(line, "set")) {
        const char *text = arg ? arg + (primary ? 8 : png ? 4 : 0) : "";
        char bytes[256];
        size_t len = strlen(text);
        if (png) {
            len = unhex(bytes, sizeof bytes, text);
            text = bytes;
        }
        int status = wl_data_clipboard_set(primary, png ? "image/png" : "text/plain;charset=utf-8", text, len);
        int *confirmed = &ds.own[primary ? 1 : 0].confirmed;
        wait_until(confirmed, 1, 1000);
        printf("set status=%d confirmed=%d\n", status, *confirmed);
    } else if (!strcmp(line, "clear")) {
        printf("clear status=%d\n", wl_data_clipboard_clear(primary));
        settle(100);
    } else if (!strcmp(line, "get")) {
        settle(100);
        print_get(primary, png ? "image/png" : "text/plain;charset=utf-8");
    } else if (!strcmp(line, "where")) {
        entered = 0;
        pointer_to(out_w / 2, out_h / 2);
        wait_until(&entered, 1, 1000);
        printf("where entered=%d x=%d y=%d w=%d h=%d\n", entered, (int)(out_w / 2 - enter_x),
               (int)(out_h / 2 - enter_y), out_w, out_h);
    } else if (!strcmp(line, "drag") && arg) {
        int x, y, x2, y2, used = 0;
        if (sscanf(arg, "%d %d %d %d %n", &x, &y, &x2, &y2, &used) < 4) {
            printf("drag usage\n");
            fflush(stdout);
            return 1;
        }
        char uris[PATH_MAX + 16];
        snprintf(uris, sizeof uris, "file://%s\r\n", arg + used);
        pointer_to(x, y);
        int before = presses_seen;
        button(1);
        int pressed = wait_until(&presses_seen, before + 1, 1000);
        int started = wl_data_drag_start(uris, strlen(uris), wl_data_press_serial());
        settle(100);
        for (int i = 1; i <= 10; i++) pointer_to(x + (x2 - x) * i / 10, y + (y2 - y) * i / 10);
        settle(200);
        button(0);
        int64_t until = now_ms() + 5000;
        while (ds.drag_active && now_ms() < until) pump(20, 0);
        settle(500);
        printf("drag pressed=%d started=%d active=%d finished=%d writes=%zu\n", pressed, started,
               ds.drag_active, ds.drag_finished, ds.nwrites);
    } else {
        printf("unknown %s\n", line);
    }
    fflush(stdout);
    return 1;
}

/* The ends a drag with a key told (unit): how many, and the last. */
static int ended_calls, ended_finished = -1;
static void ended(int finished, void *data)
{
    (void)data;
    ended_calls++;
    ended_finished = finished;
}

/* The write queue alone: partial writes, a reader that never reads (the
 * deadline), a reader gone (no SIGPIPE), and a zero-length selection; a
 * drag with a key without a compositor. */
static void unit(void)
{
    struct blob *empty = blob_new(NULL, 0);
    assert(empty && empty->size == 0);
    int p[2];
    assert(pipe2(p, O_CLOEXEC) == 0);
    queue_write(p[1], empty);
    blob_put(empty);
    struct pollfd f[WRITES_MAX];
    int n = wl_data_poll(f, WRITES_MAX);
    assert(n == 1 && poll(f, (nfds_t)n, 1000) == 1);
    wl_data_dispatch(f, (size_t)n);
    char c;
    assert(ds.nwrites == 0 && read(p[0], &c, 1) == 0);
    close(p[0]);
    puts("PASS an empty selection is an immediate end of file, not a hang");

    size_t size = 1024 * 1024;
    char *data = malloc(size), *back = malloc(size);
    assert(data && back);
    for (size_t i = 0; i < size; i++) data[i] = (char)(i * 7);
    struct blob *b = blob_new(data, size);
    assert(b && b->refs == 1);
    assert(pipe2(p, O_CLOEXEC) == 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    queue_write(p[1], b);
    assert(ds.nwrites == 1 && b->refs == 2);
    size_t got = 0;
    int rounds = 0;
    for (;;) {
        n = wl_data_poll(f, WRITES_MAX);
        poll(f, (nfds_t)n, 1000);
        wl_data_dispatch(f, (size_t)n);
        rounds++;
        ssize_t r;
        while ((r = read(p[0], back + got, size - got)) > 0) got += (size_t)r;
        if (r == 0) break;
        assert(rounds < 10000);
    }
    assert(got == size && memcmp(back, data, size) == 0 && b->refs == 1 && rounds > 1);
    close(p[0]);
    printf("PASS a 1 MiB selection is written in %d nonblocking rounds, completely, then closed\n", rounds);

    assert(pipe2(p, O_CLOEXEC) == 0);
    queue_write(p[1], b);
    ds.writes[0].until = now_ms() + 100;
    int64_t t0 = now_ms();
    while (ds.nwrites && now_ms() - t0 < 2000) {
        n = wl_data_poll(f, WRITES_MAX);
        int t = wl_data_timeout();
        assert(t >= 0 && t <= 100);
        poll(f, (nfds_t)n, t);
        wl_data_dispatch(f, (size_t)n);
    }
    int64_t took = now_ms() - t0;
    assert(ds.nwrites == 0 && took < 1000 && b->refs == 1);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    while (read(p[0], &c, 1) == 1) {}
    assert(read(p[0], &c, 1) == 0);
    close(p[0]);
    printf("PASS a reader that never reads is dropped at the deadline (%lld ms), never blocking\n", (long long)took);

    assert(pipe2(p, O_CLOEXEC) == 0);
    close(p[0]);
    queue_write(p[1], b);
    n = wl_data_poll(f, WRITES_MAX);
    poll(f, (nfds_t)n, 100);
    wl_data_dispatch(f, (size_t)n);
    sigset_t pending;
    assert(ds.nwrites == 0 && sigpending(&pending) == 0 && !sigismember(&pending, SIGPIPE) && b->refs == 1);
    puts("PASS a reader gone first ends the write quietly: no SIGPIPE delivered or left pending");

    for (int i = 0; i < WRITES_MAX + 2; i++) {
        assert(pipe2(p, O_CLOEXEC) == 0);
        queue_write(p[1], b);
        close(p[0]);
    }
    assert(ds.nwrites == WRITES_MAX && b->refs == 1 + WRITES_MAX);
    while (ds.nwrites) write_end(0);
    assert(b->refs == 1);
    puts("PASS at most 16 writes wait; more requests are closed at once, references balanced");

    blob_put(b);
    free(back);
    free(data);
    assert(mime_bit("text/plain;charset=utf-8") == M_UTF8 && kind_of("UTF8_STRING") == M_ANY_TEXT &&
           kind_of("image/png") == M_PNG && kind_of("application/x-unknown") == 0 && !blob_new("", DATA_LIMIT + 1));
    puts("PASS MIME kinds: text aliases one kind, PNG another, unknown types and oversize data refused");

    /* A drag with a document-portal key (no compositor: the source is a
     * stand-in that is never touched): each type gets its own bytes, and
     * the end is told once. */
    static const char uris[] = "file:///run/user/1000/doc/d1/a.txt\r\n", key[] = "LOCACHYDOCL2E";
    struct wl_data_source *fake = (struct wl_data_source *)&ds; /* a distinct pointer, never dereferenced */
    ds.drag = (struct offered){ .source = fake, .blob = blob_new(uris, strlen(uris)),
                                .key = blob_new(key, sizeof key), .kind = M_URI | M_KEY };
    const char *types[] = { "application/vnd.portal.filetransfer", "application/vnd.portal.files", "text/uri-list",
                            "text/plain" };
    const char *want[] = { key, key, uris, "" };
    size_t sizes[] = { sizeof key, sizeof key, strlen(uris), 0 };
    for (int i = 0; i < 4; i++) {
        assert(pipe2(p, O_CLOEXEC) == 0);
        source_send(&ds.drag, fake, types[i], p[1]);
        struct pollfd w[WRITES_MAX];
        int k = wl_data_poll(w, WRITES_MAX);
        if (k > 0) {
            assert(poll(w, (nfds_t)k, 1000) == k);
            wl_data_dispatch(w, (size_t)k);
        }
        char got[128];
        ssize_t r = read(p[0], got, sizeof got);
        assert(r == (ssize_t)sizes[i] && memcmp(got, want[i], sizes[i]) == 0 && ds.nwrites == 0);
        close(p[0]);
    }
    puts("PASS a drag with a key: both portal types get the key with its NUL, text/uri-list the list, others nothing");
    ds.drag_ended = ended;
    ds.drag.source = NULL; /* offered_clear must not destroy the stand-in */
    drag_end(1);
    offered_clear(&ds.drag);
    drag_end(0);
    assert(ended_calls == 1 && ended_finished == 1 && !ds.drag.blob && !ds.drag.key);
    ds.drag = (struct offered){ .blob = blob_new(uris, strlen(uris)), .key = blob_new(key, sizeof key) };
    ds.drag_ended = ended;
    offered_clear(&ds.drag);
    assert(ended_calls == 2 && ended_finished == 0 && !ds.drag_ended);
    puts("PASS the end of a drag with a key is told once: finished, or cancelled when it is cleared without one");
}

/* Everything this harness made, so that a leak report names wayland-data.c. */
static void teardown(void)
{
    wl_data_close();
    if (vk) zwp_virtual_keyboard_v1_destroy(vk);
    if (vp) zwlr_virtual_pointer_v1_destroy(vp);
    if (vkm) zwp_virtual_keyboard_manager_v1_destroy(vkm);
    if (vpm) zwlr_virtual_pointer_manager_v1_destroy(vpm);
    if (keyboard) wl_keyboard_destroy(keyboard);
    if (pointer) wl_pointer_destroy(pointer);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (xsurf) xdg_surface_destroy(xsurf);
    if (surface) wl_surface_destroy(surface);
    if (buffer) wl_buffer_destroy(buffer);
    if (output) wl_output_destroy(output);
    if (seat) wl_seat_destroy(seat);
    if (wm) xdg_wm_base_destroy(wm);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_DFL); /* as a program that never ignored it */
    if (argc > 1 && !strcmp(argv[1], "unit")) {
        unit();
        puts("ALL PASSED");
        return 0;
    }
    display = wl_display_connect(NULL);
    assert(display);
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display);
    assert(compositor && shm && wm && seat && vkm && vpm && ds.manager && ds.primary_manager);
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);
    wl_display_roundtrip(display);

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t ksize = strlen(text) + 1;
    int kfd = memfd_create("keymap", MFD_CLOEXEC);
    assert(kfd >= 0 && write(kfd, text, ksize) == (ssize_t)ksize);
    vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);
    zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kfd, (uint32_t)ksize);
    close(kfd);
    free(text);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, seat);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    surface = wl_compositor_create_surface(compositor);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surface);
    xdg_surface_add_listener(xsurf, &xs_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "test-wayland-data");
    xdg_toplevel_set_title(toplevel, "test-wayland-data");
    wl_surface_commit(surface);
    wait_until(&configured, 1, 3000);
    wl_data_init(display, seat, surface);
    settle(300);
    assert(wl_data_active());
    printf("ready configured=%d focused=%d\n", configured, focused);
    fflush(stdout);

    char line[PATH_MAX + 64];
    size_t len = 0;
    for (;;) {
        if (!pump(-1, 1)) continue;
        ssize_t r = read(0, line + len, sizeof line - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        char *nl;
        while ((nl = memchr(line, '\n', len))) {
            *nl = '\0';
            if (!command(line)) {
                teardown();
                return 0;
            }
            len -= (size_t)(nl + 1 - line);
            memmove(line, nl + 1, len);
        }
    }
    teardown();
    return 0;
}
