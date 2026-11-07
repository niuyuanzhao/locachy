/*
 * The window transport for the software canvas. The base draws its picture
 * as on a screen; here an xdg-shell toplevel shows it in wl_shm buffers (two,
 * one frame in flight), sized to the configured window at the preferred
 * fractional scale (wp_viewporter presents it at the logical size). The
 * seat's keyboard, with the compositor's keymap, compose sequences and
 * repeat, becomes the base's keys; its pointer becomes the base's pointer
 * events (pointer.c), in pixels of the picture. Chosen only by
 * LOCACHY_BACKEND=wayland (screen.c). Selections and dragging are the data
 * device's (wayland-data.c): it gets the connection, the seat, the surface
 * and the serials of the input events. No DRM or input device is opened.
 */
#ifdef CONFIG_WAYLAND
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "internal.h"
#include "include/wayland-backend.h"
/* The data device is part of every window build: its functions are needed
 * here (strong references), so a build without wayland-data.o fails to link
 * instead of calling a missing function. */
#define WL_DATA_IMPLEMENTATION
#include "include/wayland-data.h"
#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#define MAX_SIDE 16384        /* pixels of a buffer's side, as a bound on its memory */
#define DEFAULT_W 1100        /* the window's first logical size, when the compositor leaves it to us */
#define DEFAULT_H 760
#define KEYS 64               /* keys typed and not read yet */
#define DATA_FDS 16           /* transfers of the data device polled at once */
#define SCROLL_PIXELS 15.0    /* finger and continuous scrolling per wheel step (pointer.c's) */
#define KEYMAP_MAX (1 << 20)
#define CLOSE_AGAIN_S 3.0     /* a second close this soon after an unanswered K_CLOSE ends the program */

/* A close request becomes K_CLOSE; the program answers by leaving, or by
 * key_close_kept() when it stays open on purpose. */
enum { CLOSE_NONE, CLOSE_QUEUED, CLOSE_READ };

static double monotonic(void);

struct buffer {
    struct wl_buffer *buffer;
    uint32_t *pixels;
    size_t size;
    int width, height, busy;
};

struct key {
    int key, mods;
    char text[64], name[64];
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wp_fractional_scale_manager_v1 *fractional;
    struct wp_viewporter *viewporter;
    struct zxdg_decoration_manager_v1 *decorations;
    struct wl_seat *seat;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    int decoration_mode;      /* the compositor's answer: 0 none yet, 1 client side, 2 server side */
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *scale_object;
    struct wl_callback *frame;
    struct buffer buffers[2];

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose;
    int mods, repeat_fd, repeat_rate, repeat_delay;
    uint32_t repeat_code, keyboard_serial, button_serial, enter_serial;

    struct wl_cursor_theme *cursor_theme;
    int cursor_size, cursor_shape;
    struct wl_surface *cursor_surface;

    int (*resize)(int width, int height, double scale);
    void (*on_frame)(void);
    int configured, closed, held, frame_due, ready_told, name_keys;
    int close_state;          /* CLOSE_*: the close requests' K_CLOSE */
    double close_read_at;     /* when the program read the last K_CLOSE */
    int pending_w, pending_h, bounds_w, bounds_h, logical_w, logical_h, pixel_w, pixel_h;
    int integer_scale, buffer_scale_set;
    double fractional_scale, scale;
    unsigned events;          /* what a caller would see changed: keys, pointer, size, close */

    struct {
        double v, h, v120, h120;
        int dv, dh, source, axes, value120;
    } axis;
} w = { .repeat_fd = -1, .integer_scale = 1, .cursor_shape = CURSOR_ARROW };

static struct key keyq[KEYS], last_key;
static int khead, klen;

struct wl_display *wl_backend_display(void) { return w.display; }
struct wl_seat *wl_backend_seat(void) { return w.seat; }
struct wl_surface *wl_backend_surface(void) { return w.surface; }
uint32_t wl_backend_keyboard_serial(void) { return w.keyboard_serial; }
uint32_t wl_backend_button_serial(void) { return w.button_serial; }
int wl_backend_active(void) { return w.display != NULL && w.configured; }
int wl_backend_modifiers(void) { return last_key.mods; }
const char *wl_backend_text(void) { return last_key.text; }
const char *wl_backend_key_name(void) { return last_key.name; }
void wl_backend_name_keys(int on) { w.name_keys = !!on; }

int wl_backend_next_key(void)
{
    if (klen == 0) return 0;
    last_key = keyq[khead];
    khead = (khead + 1) % KEYS;
    klen--;
    if (last_key.key == K_CLOSE) {
        w.close_state = CLOSE_READ;
        w.close_read_at = monotonic();
    }
    return last_key.key;
}

void wl_backend_close_kept(void)
{
    if (w.close_state == CLOSE_READ) w.close_state = CLOSE_NONE;
}

int wl_backend_close_forced(void)
{
    return w.close_state == CLOSE_READ && w.closed;
}

int wl_backend_decoration(void)
{
    return w.decoration_mode;
}

/* ---- the keyboard ---- */

static int modifier_sym(xkb_keysym_t sym)
{
    return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
           (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) ||
           sym == XKB_KEY_Mode_switch || sym == XKB_KEY_Num_Lock;
}

/* The name a binding gives it, as keys_last_name has them: Space, Enter,
 * Tab, a letter or digit in capitals, else the keysym's. */
static void key_name(xkb_keysym_t sym, char *out, size_t size)
{
    uint32_t c = xkb_keysym_to_utf32(sym);
    if (sym == XKB_KEY_space) snprintf(out, size, "Space");
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) snprintf(out, size, "Enter");
    else if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) snprintf(out, size, "Tab");
    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        snprintf(out, size, "%c", (char)(c >= 'a' && c <= 'z' ? c - 32 : c));
    else if (xkb_keysym_get_name(sym, out, size) < 0) out[0] = '\0';
}

static void put_key(int key, const char *text, xkb_keysym_t sym)
{
    if (klen == KEYS) { /* the oldest goes, as a full evdev queue drops */
        khead = (khead + 1) % KEYS;
        klen--;
    }
    struct key *k = &keyq[(khead + klen++) % KEYS];
    k->key = key;
    k->mods = w.mods;
    snprintf(k->text, sizeof k->text, "%s", text ? text : "");
    if (sym != XKB_KEY_NoSymbol) key_name(sym, k->name, sizeof k->name);
    else k->name[0] = '\0';
    w.events++;
}

/* The keys that are not text, as keys.c's translate gives them. */
static int special_key(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Escape: return 27;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return '\n';
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return '\t';
    case XKB_KEY_BackSpace: return '\b';
    case XKB_KEY_Up: case XKB_KEY_KP_Up: return K_UP;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: return K_DOWN;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: return K_LEFT;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: return K_RIGHT;
    case XKB_KEY_Prior: case XKB_KEY_KP_Prior: return K_PGUP;
    case XKB_KEY_Next: case XKB_KEY_KP_Next: return K_PGDN;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: return K_BOL;
    case XKB_KEY_End: case XKB_KEY_KP_End: return K_EOL;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return K_DEL;
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return K_INS;
    }
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) return K_F1 + (int)(sym - XKB_KEY_F1);
    return 0;
}

static void key_pressed(uint32_t code)
{
    if (!w.state) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(w.state, code + 8);
    if (sym == XKB_KEY_NoSymbol) return;
    int ctrl = w.mods & KEYS_CTRL, alt = w.mods & KEYS_ALT, meta = w.mods & KEYS_META, shift = w.mods & KEYS_SHIFT;
    int bare = !ctrl && !alt && !meta;
    int special = special_key(sym);
    if (special) {
        if (w.compose) xkb_compose_state_reset(w.compose);
        put_key(special, "", sym);
        return;
    }
    if (ctrl && !alt && !shift && sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
        put_key(K_CTRL_0 + (int)(sym - XKB_KEY_0), "", sym);
        return;
    }
    uint32_t c = xkb_keysym_to_utf32(sym);
    if (ctrl && (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '[' && c <= '_'))) {
        put_key((int)(c & 0x1F), "", sym);
        return;
    }
    char text[64] = "";
    if (bare && w.compose && xkb_compose_state_feed(w.compose, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(w.compose)) {
        case XKB_COMPOSE_COMPOSING:
            return;
        case XKB_COMPOSE_COMPOSED:
            xkb_compose_state_get_utf8(w.compose, text, sizeof text);
            xkb_compose_state_reset(w.compose);
            break;
        case XKB_COMPOSE_CANCELLED:
            xkb_compose_state_reset(w.compose);
            return;
        case XKB_COMPOSE_NOTHING:
            break;
        }
    }
    if (!text[0]) xkb_state_key_get_utf8(w.state, code + 8, text, sizeof text);
    unsigned char first = (unsigned char)text[0];
    if (first >= 0x20 && first < 0x7F && text[1] == '\0') {
        put_key(first, text, sym);
    } else if (first >= 0x80 && bare) {
        put_key(K_TEXT, text, sym);
    } else if (!first && w.name_keys && !modifier_sym(sym)) {
        put_key(K_NAMED, "", sym);
    }
}

static void stop_repeat(void)
{
    w.repeat_code = 0;
    if (w.repeat_fd >= 0) timerfd_settime(w.repeat_fd, 0, &(struct itimerspec){ 0 }, NULL);
}

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kb;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > KEYMAP_MAX) {
        close(fd);
        return;
    }
    char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED) return;
    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(w.xkb, text, strnlen(text, size),
                                                            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (!keymap) {
        say("window: the compositor's keymap cannot be compiled; keys stay as they were");
        return;
    }
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }
    xkb_state_unref(w.state);
    xkb_keymap_unref(w.keymap);
    w.keymap = keymap;
    w.state = state;
    stop_repeat();
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data; (void)kb; (void)surface; (void)keys;
    w.keyboard_serial = serial;
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)surface;
    w.keyboard_serial = serial;
    stop_repeat();
    if (w.compose) xkb_compose_state_reset(w.compose);
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t code,
                         uint32_t pressed)
{
    (void)data; (void)kb; (void)time;
    w.keyboard_serial = serial;
    if (pressed != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (code == w.repeat_code) stop_repeat();
        return;
    }
    key_pressed(code);
    if (w.keymap && w.repeat_rate > 0 && xkb_keymap_key_repeats(w.keymap, code + 8) && w.repeat_fd >= 0) {
        long interval = 1000000000L / w.repeat_rate;
        struct itimerspec t = {
            .it_value = { w.repeat_delay / 1000, (long)(w.repeat_delay % 1000) * 1000000L },
            .it_interval = { interval / 1000000000L, interval % 1000000000L },
        };
        if (t.it_value.tv_sec == 0 && t.it_value.tv_nsec == 0) t.it_value.tv_nsec = 1;
        w.repeat_code = code;
        timerfd_settime(w.repeat_fd, 0, &t, NULL);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)kb;
    w.keyboard_serial = serial;
    if (!w.state) return;
    xkb_state_update_mask(w.state, depressed, latched, locked, 0, 0, group);
    w.mods = 0;
    if (xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) w.mods |= KEYS_SHIFT;
    if (xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) w.mods |= KEYS_CTRL;
    if (xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) w.mods |= KEYS_ALT;
    if (xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) w.mods |= KEYS_META;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data; (void)kb;
    w.repeat_rate = rate > 0 && rate <= 1000 ? rate : 0;
    w.repeat_delay = delay > 0 && delay <= 10000 ? delay : 600;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ---- the pointer ---- */

static int picture_x(wl_fixed_t x)
{
    return w.logical_w > 0 ? (int)(wl_fixed_to_double(x) * w.pixel_w / w.logical_w) : wl_fixed_to_int(x);
}

static int picture_y(wl_fixed_t y)
{
    return w.logical_h > 0 ? (int)(wl_fixed_to_double(y) * w.pixel_h / w.logical_h) : wl_fixed_to_int(y);
}

/* The cursor the base asked for, at the window's scale (in whole pixels:
 * cursor themes come in integer sizes). */
static void show_cursor(void)
{
    if (!w.pointer || !w.enter_serial) return;
    if (w.cursor_shape == CURSOR_NONE) {
        wl_pointer_set_cursor(w.pointer, w.enter_serial, NULL, 0, 0);
        return;
    }
    int s = (int)ceil(w.scale > 0 ? w.scale : 1.0);
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    const char *size_text = getenv("XCURSOR_SIZE");
    int size = (size_text && atoi(size_text) > 0 && atoi(size_text) <= 128 ? atoi(size_text) : 24) * s;
    if (!w.cursor_theme || w.cursor_size != size) {
        if (w.cursor_theme) wl_cursor_theme_destroy(w.cursor_theme);
        w.cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, w.shm);
        w.cursor_size = size;
    }
    if (!w.cursor_theme) return;
    const char *const text_names[] = { "xterm", "text", NULL }, *const arrow_names[] = { "left_ptr", "default", NULL };
    const char *const *names = w.cursor_shape == CURSOR_TEXT ? text_names : arrow_names;
    struct wl_cursor *cursor = NULL;
    for (int i = 0; names[i] && !cursor; i++) cursor = wl_cursor_theme_get_cursor(w.cursor_theme, names[i]);
    if (!cursor || cursor->image_count == 0) return;
    struct wl_cursor_image *image = cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    if (!w.cursor_surface) w.cursor_surface = wl_compositor_create_surface(w.compositor);
    if (!w.cursor_surface) return;
    wl_surface_set_buffer_scale(w.cursor_surface, s);
    wl_surface_attach(w.cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(w.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(w.cursor_surface);
    wl_pointer_set_cursor(w.pointer, w.enter_serial, w.cursor_surface,
                          (int32_t)image->hotspot_x / s, (int32_t)image->hotspot_y / s);
}

void wl_backend_cursor(int shape)
{
    if (shape == w.cursor_shape) return;
    w.cursor_shape = shape;
    show_cursor();
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)surface;
    w.enter_serial = serial;
    show_cursor();
    pointer_window_motion(picture_x(x), picture_y(y));
    w.events++;
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)p; (void)serial; (void)surface;
    pointer_window_leave();
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time;
    pointer_window_motion(picture_x(x), picture_y(y));
    w.events++;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data; (void)p; (void)time;
    w.button_serial = serial;
    if (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE) return;
    pointer_window_button((int)button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    w.events++;
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)p; (void)time;
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        w.axis.v += v;
        w.axis.axes |= 1;
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        w.axis.h += v;
        w.axis.axes |= 2;
    }
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
    (void)data; (void)p;
    w.axis.source = source == WL_POINTER_AXIS_SOURCE_FINGER ? SCROLL_FINGER :
                    source == WL_POINTER_AXIS_SOURCE_CONTINUOUS ? SCROLL_CONTINUOUS : SCROLL_WHEEL;
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
    (void)data; (void)p; (void)time;
    w.axis.axes |= axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 1 : 2;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
    (void)data; (void)p;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) w.axis.dv += discrete;
    else w.axis.dh += discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value120)
{
    (void)data; (void)p;
    w.axis.value120 = 1;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) w.axis.v120 += value120;
    else w.axis.h120 += value120;
}

static void pointer_axis_direction(void *data, struct wl_pointer *p, uint32_t axis, uint32_t direction)
{
    (void)data; (void)p; (void)axis; (void)direction;
}

/* One frame's scrolling: a wheel's in its own steps (high resolution ones
 * as parts), a touchpad's or TrackPoint's in SCROLL_PIXELS a step. */
static void pointer_frame(void *data, struct wl_pointer *p)
{
    (void)data; (void)p;
    if (!w.axis.axes) return;
    int source = w.axis.source ? w.axis.source : SCROLL_WHEEL;
    double v, h;
    if (source == SCROLL_WHEEL) {
        v = w.axis.value120 ? w.axis.v120 / 120.0 : w.axis.dv ? w.axis.dv : w.axis.v / SCROLL_PIXELS;
        h = w.axis.value120 ? w.axis.h120 / 120.0 : w.axis.dh ? w.axis.dh : w.axis.h / SCROLL_PIXELS;
    } else {
        v = w.axis.v / SCROLL_PIXELS;
        h = w.axis.h / SCROLL_PIXELS;
    }
    pointer_window_scroll(source, v, h, w.axis.axes);
    memset(&w.axis, 0, sizeof w.axis);
    w.events++;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_direction,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w.keyboard) {
        w.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w.keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && w.keyboard) {
        stop_repeat();
        wl_keyboard_release(w.keyboard);
        w.keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w.pointer) {
        w.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w.pointer, &pointer_listener, NULL);
        pointer_window_present(1);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && w.pointer) {
        wl_pointer_release(w.pointer);
        w.pointer = NULL;
        w.enter_serial = 0;
        pointer_window_present(0);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- the window and its size ---- */

/* The configured logical size at the preferred scale, in buffer pixels; the
 * base's picture follows it (resize), told to the program as K_SHOWN. */
static void apply_size(void)
{
    /* Left to us: the last size, else a default that fits the bounds the
     * compositor recommends (the output less its panels). */
    int first_w = w.bounds_w > 0 && w.bounds_w < DEFAULT_W ? w.bounds_w : DEFAULT_W;
    int first_h = w.bounds_h > 0 && w.bounds_h < DEFAULT_H ? w.bounds_h : DEFAULT_H;
    int lw = w.pending_w > 0 ? w.pending_w : w.logical_w > 0 ? w.logical_w : first_w;
    int lh = w.pending_h > 0 ? w.pending_h : w.logical_h > 0 ? w.logical_h : first_h;
    double s = w.viewport && w.fractional_scale > 0 ? w.fractional_scale : w.integer_scale;
    if (s < 0.5 || s > 8) s = 1.0;
    double pw = floor(lw * s + 0.5), ph = floor(lh * s + 0.5);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    if (pw > MAX_SIDE) pw = MAX_SIDE;
    if (ph > MAX_SIDE) ph = MAX_SIDE;
    if ((int)pw == w.pixel_w && (int)ph == w.pixel_h && s == w.scale && lw == w.logical_w && lh == w.logical_h) {
        return;
    }
    if (w.resize && w.resize((int)pw, (int)ph, s) < 0) {
        say("window: no picture of %dx%d pixels; the window keeps its size", (int)pw, (int)ph);
        return;
    }
    w.logical_w = lw;
    w.logical_h = lh;
    w.pixel_w = (int)pw;
    w.pixel_h = (int)ph;
    w.scale = s;
    w.events++;
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data; (void)toplevel; (void)states;
    w.pending_w = width;
    w.pending_h = height;
}

/* The close button or the compositor's close binding: K_CLOSE for the
 * program's own quit, once while it waits to be read. Asked again within
 * CLOSE_AGAIN_S of the program reading it, with no key_close_kept() since,
 * the program does not know K_CLOSE (or is stuck): it ends, as before. */
static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data; (void)toplevel;
    if (w.close_state == CLOSE_QUEUED) return;
    if (w.close_state == CLOSE_READ && monotonic() - w.close_read_at < CLOSE_AGAIN_S) {
        w.closed = 1;
        w.events++;
        return;
    }
    w.close_state = CLOSE_QUEUED;
    put_key(K_CLOSE, "", XKB_KEY_NoSymbol);
}

static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode)
{
    (void)data; (void)decoration;
    w.decoration_mode = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? 2 : 1;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height)
{
    (void)data; (void)toplevel;
    w.bounds_w = width > 0 ? width : 0;
    w.bounds_h = height > 0 ? height : 0;
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps)
{
    (void)data; (void)toplevel; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    w.configured = 1;
    apply_size();
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *object, uint32_t scale)
{
    (void)data; (void)object;
    w.fractional_scale = scale / 120.0;
    if (w.configured) apply_size();
}

static const struct wp_fractional_scale_v1_listener fractional_listener = { .preferred_scale = preferred_scale };

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface; (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface; (void)output;
}

static void surface_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)data; (void)surface;
    w.integer_scale = factor >= 1 && factor <= 8 ? factor : 1;
    if (w.configured) apply_size();
}

static void surface_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data; (void)surface; (void)transform;
}

static const struct wl_surface_listener surface_events = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_buffer_scale,
    .preferred_buffer_transform = surface_buffer_transform,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0 && !w.compositor && version >= 4) {
        w.compositor_version = version < 6 ? version : 6;
        w.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, w.compositor_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0 && !w.shm) {
        w.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !w.wm_base) {
        w.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 5 ? version : 5);
        xdg_wm_base_add_listener(w.wm_base, &wm_listener, NULL);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 && !w.fractional) {
        w.fractional = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 && !w.decorations) {
        w.decorations = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0 && !w.viewporter) {
        w.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !w.seat) {
        w.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 8 ? version : 8);
        wl_seat_add_listener(w.seat, &seat_listener, NULL);
    }
    wl_data_global(registry, name, interface, version);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ---- buffers and frames ---- */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    struct buffer *b = data;
    b->busy = 0;
    if (w.held && !w.frame) w.frame_due = 1;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void buffer_free(struct buffer *b)
{
    if (b->buffer) wl_buffer_destroy(b->buffer);
    if (b->pixels) munmap(b->pixels, b->size);
    memset(b, 0, sizeof *b);
}

static int buffer_make(struct buffer *b, int width, int height)
{
    if (b->buffer && b->width == width && b->height == height) return 0;
    buffer_free(b);
    size_t stride = (size_t)width * 4, size = stride * (size_t)height;
    int fd = memfd_create("locachy-window", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    /* The compositor maps it too: it cannot shrink under either side. */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(w.shm, fd, (int32_t)size);
    close(fd);
    b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    b->pixels = map;
    b->size = size;
    b->width = width;
    b->height = height;
    b->busy = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)data; (void)time;
    wl_callback_destroy(callback);
    w.frame = NULL;
    if (w.held) w.frame_due = 1;
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* The launcher's readiness pipe: the window shows its first picture. */
static void tell_ready(void)
{
    const char *text = getenv("LOCACHY_WINDOW_READY_FD");
    if (!text) return;
    char *end;
    long fd = strtol(text, &end, 10);
    unsetenv("LOCACHY_WINDOW_READY_FD");
    if (*text == '\0' || *end != '\0' || fd < 3 || fd > 1023) return;
    while (write((int)fd, "1", 1) < 0 && errno == EINTR) {
    }
    close((int)fd);
}

int wl_backend_flush(const uint32_t *pixels, int pw, int ph, int x0, int y0, int x1, int y1)
{
    if (!w.display || !w.surface || w.closed || !pixels || pw < 1 || ph < 1 || pw > MAX_SIDE || ph > MAX_SIDE) {
        return -1;
    }
    if (w.frame) {
        w.held = 1;
        return 1;
    }
    struct buffer *b = NULL;
    for (int i = 0; i < 2 && !b; i++) {
        if (!w.buffers[i].busy && w.buffers[i].width == pw && w.buffers[i].height == ph) b = &w.buffers[i];
    }
    for (int i = 0; i < 2 && !b; i++) {
        if (!w.buffers[i].busy) b = &w.buffers[i];
    }
    if (!b) {
        w.held = 1;
        return 1;
    }
    if (buffer_make(b, pw, ph) < 0) {
        say("window: no %dx%d buffer: %s", pw, ph, strerror(errno));
        return -1;
    }
    memcpy(b->pixels, pixels, (size_t)pw * (size_t)ph * 4);
    b->busy = 1;
    wl_surface_attach(w.surface, b->buffer, 0, 0);
    if (w.viewport) {
        wp_viewport_set_destination(w.viewport, w.logical_w, w.logical_h);
    } else if (w.buffer_scale_set != w.integer_scale) {
        wl_surface_set_buffer_scale(w.surface, w.integer_scale);
        w.buffer_scale_set = w.integer_scale;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pw) x1 = pw;
    if (y1 > ph) y1 = ph;
    if (x0 >= x1 || y0 >= y1) {
        x0 = y0 = 0;
        x1 = pw;
        y1 = ph;
    }
    wl_surface_damage_buffer(w.surface, x0, y0, x1 - x0, y1 - y0);
    w.frame = wl_surface_frame(w.surface);
    wl_callback_add_listener(w.frame, &frame_listener, NULL);
    wl_surface_commit(w.surface);
    w.held = 0;
    wl_display_flush(w.display);
    if (!w.ready_told) {
        w.ready_told = 1;
        tell_ready();
    }
    return 0;
}

/* ---- opening, waiting, closing ---- */

static double monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void open_compose(void)
{
    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_CTYPE");
    if (!locale || !*locale) locale = getenv("LANG");
    if (!locale || !*locale) locale = "C";
    w.compose_table = xkb_compose_table_new_from_locale(w.xkb, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!w.compose_table && strcmp(locale, "C") != 0) {
        w.compose_table = xkb_compose_table_new_from_locale(w.xkb, "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    if (w.compose_table) w.compose = xkb_compose_state_new(w.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
}

int wl_backend_open(int (*resize)(int width, int height, double scale), void (*frame)(void))
{
    const char *ready = getenv("LOCACHY_WINDOW_READY_FD");
    if (ready) { /* the renderer and other children never inherit it */
        long fd = strtol(ready, NULL, 10);
        if (fd >= 3 && fd <= 1023) fcntl((int)fd, F_SETFD, FD_CLOEXEC);
    }
    w.resize = resize;
    w.on_frame = frame;
    w.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    w.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    w.display = wl_display_connect(NULL);
    if (!w.xkb || w.repeat_fd < 0 || !w.display) {
        wl_backend_close();
        return -1;
    }
    open_compose();
    w.registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(w.registry, &registry_listener, NULL);
    if (wl_display_roundtrip(w.display) < 0 || !w.compositor || !w.shm || !w.wm_base) {
        say("window: the compositor lacks wl_compositor 4, wl_shm or xdg_wm_base");
        wl_backend_close();
        return -1;
    }
    w.surface = wl_compositor_create_surface(w.compositor);
    if (w.compositor_version >= 6) wl_surface_add_listener(w.surface, &surface_events, NULL);
    if (w.viewporter) w.viewport = wp_viewporter_get_viewport(w.viewporter, w.surface);
    if (w.fractional && w.viewport) {
        w.scale_object = wp_fractional_scale_manager_v1_get_fractional_scale(w.fractional, w.surface);
        wp_fractional_scale_v1_add_listener(w.scale_object, &fractional_listener, NULL);
    }
    w.xdg_surface = xdg_wm_base_get_xdg_surface(w.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg_surface, &surface_listener, NULL);
    w.toplevel = xdg_surface_get_toplevel(w.xdg_surface);
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, NULL);
    /* The compositor's frame (title, close and maximize buttons): labwc draws
     * none for an xdg window that does not ask for it. Asked before the
     * first commit, so the first configure already has it. */
    if (w.decorations) {
        w.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(w.decorations, w.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(w.decoration, &decoration_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(w.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    const char *title = getenv("LOCACHY_WINDOW_TITLE"), *app_id = getenv("LOCACHY_WINDOW_APP_ID");
    xdg_toplevel_set_title(w.toplevel, title && *title ? title : "LOCachy");
    xdg_toplevel_set_app_id(w.toplevel, app_id && *app_id ? app_id : "locachy");
    xdg_toplevel_set_min_size(w.toplevel, 320, 200);
    if (w.seat) wl_data_init(w.display, w.seat, w.surface);
    wl_surface_commit(w.surface); /* no buffer: asks for the first configure */
    double deadline = monotonic() + 10.0;
    while (!w.configured && !w.closed) {
        int left = (int)((deadline - monotonic()) * 1000);
        if (left <= 0) break;
        while (wl_display_prepare_read(w.display) != 0) {
            if (wl_display_dispatch_pending(w.display) < 0) break;
        }
        wl_display_flush(w.display);
        struct pollfd p = { .fd = wl_display_get_fd(w.display), .events = POLLIN };
        int r = poll(&p, 1, left);
        if (r > 0) {
            if (wl_display_read_events(w.display) < 0) break;
        } else {
            wl_display_cancel_read(w.display);
            if (r < 0 && errno != EINTR) break;
        }
        if (wl_display_dispatch_pending(w.display) < 0) break;
    }
    if (!w.configured || w.closed || w.pixel_w < 1) {
        say("window: the compositor did not configure the window");
        wl_backend_close();
        return -1;
    }
    return 0;
}

int wl_backend_wait(struct pollfd *extra, size_t count, int timeout_ms)
{
    if (!w.display || w.closed) return -1;
    if (count > 64) count = 64;
    if (w.frame_due) {
        w.frame_due = 0;
        if (w.on_frame) w.on_frame();
    }
    unsigned before = w.events;
    while (wl_display_prepare_read(w.display) != 0) {
        if (wl_display_dispatch_pending(w.display) < 0) return -1;
    }
    if (w.events != before || w.closed) {
        wl_display_cancel_read(w.display);
        return w.closed ? -1 : 1;
    }
    short out = 0;
    if (wl_display_flush(w.display) < 0) {
        if (errno != EAGAIN) {
            wl_display_cancel_read(w.display);
            return -1;
        }
        out = POLLOUT;
    }
    struct pollfd p[2 + 64 + DATA_FDS];
    p[0] = (struct pollfd){ .fd = wl_display_get_fd(w.display), .events = POLLIN | out };
    p[1] = (struct pollfd){ .fd = w.repeat_fd, .events = POLLIN };
    for (size_t i = 0; i < count; i++) {
        p[2 + i] = extra[i];
        p[2 + i].revents = 0;
    }
    int ndata = wl_data_poll(p + 2 + count, DATA_FDS);
    if (ndata < 0) ndata = 0;
    if (ndata > DATA_FDS) ndata = DATA_FDS;
    int timeout = timeout_ms, data_timeout = wl_data_timeout(), shortened = 0;
    if (data_timeout >= 0 && (timeout < 0 || data_timeout < timeout)) {
        timeout = data_timeout;
        shortened = 1;
    }
    int r = poll(p, (nfds_t)(2 + count + (size_t)ndata), timeout);
    if (r < 0) {
        int error = errno;
        wl_display_cancel_read(w.display);
        return error == EINTR ? 1 : -1;
    }
    if (p[0].revents & (POLLIN | POLLERR | POLLHUP)) {
        if (wl_display_read_events(w.display) < 0) return -1;
    } else {
        wl_display_cancel_read(w.display);
    }
    if (p[1].revents & POLLIN) {
        uint64_t expired = 0;
        if (read(w.repeat_fd, &expired, sizeof expired) == (ssize_t)sizeof expired && w.repeat_code) {
            for (uint64_t i = 0; i < expired && i < 4; i++) key_pressed(w.repeat_code);
        }
    }
    if (wl_display_dispatch_pending(w.display) < 0) return -1;
    if (ndata > 0) wl_data_dispatch(p + 2 + count, (size_t)ndata);
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        extra[i].revents = p[2 + i].revents;
        ready |= extra[i].revents != 0;
    }
    if (w.closed) return -1;
    if (w.frame_due) {
        w.frame_due = 0;
        if (w.on_frame) w.on_frame();
    }
    if (w.events != before) return 1;
    if (ready) return 2;
    return r == 0 && !shortened ? 0 : 1;
}

void wl_backend_close(void)
{
    if (w.display) wl_data_close();
    stop_repeat();
    if (w.frame) wl_callback_destroy(w.frame);
    for (int i = 0; i < 2; i++) buffer_free(&w.buffers[i]);
    if (w.cursor_surface) wl_surface_destroy(w.cursor_surface);
    if (w.cursor_theme) wl_cursor_theme_destroy(w.cursor_theme);
    if (w.pointer) wl_pointer_release(w.pointer);
    if (w.keyboard) wl_keyboard_release(w.keyboard);
    if (w.decoration) zxdg_toplevel_decoration_v1_destroy(w.decoration); /* before its toplevel */
    if (w.toplevel) xdg_toplevel_destroy(w.toplevel);
    if (w.xdg_surface) xdg_surface_destroy(w.xdg_surface);
    if (w.scale_object) wp_fractional_scale_v1_destroy(w.scale_object);
    if (w.viewport) wp_viewport_destroy(w.viewport);
    if (w.surface) wl_surface_destroy(w.surface);
    if (w.seat) wl_seat_release(w.seat);
    if (w.fractional) wp_fractional_scale_manager_v1_destroy(w.fractional);
    if (w.viewporter) wp_viewporter_destroy(w.viewporter);
    if (w.decorations) zxdg_decoration_manager_v1_destroy(w.decorations);
    if (w.wm_base) xdg_wm_base_destroy(w.wm_base);
    if (w.shm) wl_shm_destroy(w.shm);
    if (w.compositor) wl_compositor_destroy(w.compositor);
    if (w.registry) wl_registry_destroy(w.registry);
    if (w.display) wl_display_disconnect(w.display);
    if (w.repeat_fd >= 0) close(w.repeat_fd);
    xkb_compose_state_unref(w.compose);
    xkb_compose_table_unref(w.compose_table);
    xkb_state_unref(w.state);
    xkb_keymap_unref(w.keymap);
    xkb_context_unref(w.xkb);
    memset(&w, 0, sizeof w);
    w.repeat_fd = -1;
    w.integer_scale = 1;
    w.cursor_shape = CURSOR_ARROW;
    khead = klen = 0;
}
#else
/* Direct-only builds: no window transport (screen.c refuses a request). */
typedef int wayland_backend_not_built;
#endif
