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
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define MAX_SIDE 16384        /* pixels of a buffer's side, as a bound on its memory */
#define DEFAULT_W 1100        /* the window's first logical size, when the compositor leaves it to us */
#define DEFAULT_H 760
#define FLOAT_W 760           /* centered floating overlay logical size (LOCACHY_WINDOW_FLOATING=1) */
#define FLOAT_H 480
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
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
};

struct key {
    int key, mods;
    uint32_t code;
    uint8_t is_repeat;
    char text[64], name[64];
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_fractional_scale_manager_v1 *fractional;
    struct wp_viewporter *viewporter;
    struct zxdg_decoration_manager_v1 *decorations;
    struct wl_seat *seat;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct zxdg_toplevel_decoration_v1 *decoration;
    int decoration_mode;      /* the compositor's answer: 0 none yet, 1 client side, 2 server side */
    int floating;             /* 1: centered always-on-top overlay (LOCACHY_WINDOW_FLOATING=1) */
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *scale_object;
    struct wl_callback *frame;
    struct buffer buffers[2];

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose;
    int mods, use_key_mods, keyboard_focused, repeat_fd, repeat_rate, repeat_delay;
    unsigned keyboard_focus_generation;
    uint32_t repeat_code, keyboard_serial, button_serial, press_serial, enter_serial;
    int theme_fd, theme_dirty;  /* rc.conf replaced or edited; request a new picture */

    struct wl_cursor_theme *cursor_theme;
    int cursor_size, cursor_shape;
    struct wl_surface *cursor_surface;
    struct wp_viewport *cursor_viewport;

    int (*resize)(int width, int height, double scale);
    void (*on_frame)(void);
    int configured, closed, held, frame_due, ready_told, name_keys;
    int close_state;          /* CLOSE_*: the close requests' K_CLOSE */
    double close_read_at;     /* when the program read the last K_CLOSE */
    int pending_w, pending_h, bounds_w, bounds_h, logical_w, logical_h, pixel_w, pixel_h;
    int integer_scale, buffer_scale_set;
    double fractional_scale, scale;
    unsigned events;          /* what a caller would see changed: keys, pointer, size, close */

    int last_axis_source, active_axes;
    struct {
        double v, h, v120, h120;
        int dv, dh, source, axes, value120_axes;
    } axis;
} w = { .repeat_fd = -1, .theme_fd = -1, .integer_scale = 1, .cursor_shape = CURSOR_ARROW };

static struct key keyq[KEYS], last_key;
static int khead, klen;

struct wl_display *wl_backend_display(void) { return w.display; }
struct wl_seat *wl_backend_seat(void) { return w.seat; }
struct wl_surface *wl_backend_surface(void) { return w.surface; }
uint32_t wl_backend_keyboard_serial(void) { return w.keyboard_serial; }
uint32_t wl_backend_button_serial(void) { return w.button_serial; }
uint32_t wl_backend_press_serial(void) { return w.press_serial; }
int wl_backend_keyboard_focused(void) { return w.keyboard_focused; }
unsigned wl_backend_keyboard_focus_generation(void) { return w.keyboard_focus_generation; }
int wl_backend_active(void) { return w.display != NULL && w.configured; }
int wl_backend_theme_changed(void)
{
    if (!w.theme_dirty) return 0;
    w.theme_dirty = 0;
    return 1;
}
int wl_backend_modifiers(void) { return w.use_key_mods ? last_key.mods : w.mods; }
void wl_backend_live_modifiers(void) { w.use_key_mods = 0; }
const char *wl_backend_text(void) { return last_key.text; }
const char *wl_backend_key_name(void) { return last_key.name; }
void wl_backend_name_keys(int on) { w.name_keys = !!on; }

int wl_backend_next_key(void)
{
    if (klen == 0) return 0;
    while (klen) {
        last_key = keyq[khead];
        khead = (khead + 1) % KEYS;
        klen--;
        /* K_CLOSE is a window request, not a keyboard event. */
        if (last_key.key == K_CLOSE || !base_raw_exclusive || !base_raw_exclusive()) break;
        last_key.key = 0;
    }
    if (!last_key.key) return 0;
    w.use_key_mods = 1;
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

static void put_key(int key, const char *text, xkb_keysym_t sym, uint32_t code, int is_repeat)
{
    if (klen == KEYS) { /* the oldest goes, as a full evdev queue drops */
        khead = (khead + 1) % KEYS;
        klen--;
    }
    struct key *k = &keyq[(khead + klen++) % KEYS];
    k->key = key;
    k->mods = w.mods;
    k->code = code;
    k->is_repeat = is_repeat ? 1 : 0;
    snprintf(k->text, sizeof k->text, "%s", text ? text : "");
    if (sym != XKB_KEY_NoSymbol) key_name(sym, k->name, sizeof k->name);
    else k->name[0] = '\0';
    w.events++;
}

static void purge_repeats(uint32_t code)
{
    int kept = 0;
    for (int i = 0; i < klen; i++) {
        struct key item = keyq[(khead + i) % KEYS];
        if (item.is_repeat && item.code == code) continue;
        keyq[(khead + kept++) % KEYS] = item;
    }
    klen = kept;
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

/* base_raw_emit derives momentary modifiers from the keys held in the raw
 * session before this event, including on release and repeat. Only pass lock
 * bits from the compositor's current mask, as on evdev: a key that changes
 * them is reported before the change. Do not advance the client XKB state;
 * wl_keyboard.modifiers owns it for local text and actions. */
static void raw_key(uint32_t code, xkb_keysym_t sym, uint32_t state, int local)
{
    int caps = xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
    int num = xkb_state_mod_name_is_active(w.state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0;
    uint32_t locks = (caps ? 1u : 0u) | (num ? 2u : 0u);
    uint32_t mods = (caps ? KEYS_CAPS : 0u) | (num ? 64u : 0u) |
                    (xkb_state_mod_name_is_active(w.state, "Mod5", XKB_STATE_MODS_EFFECTIVE) > 0 ?
                     (uint32_t)KEYS_ALTGR : 0u);
    base_raw_emit(BASE_RAW_WAYLAND_DEVICE, code, sym, mods, locks, state, local);
}

static void key_pressed(uint32_t code, int is_repeat)
{
    if (!w.state || (base_raw_exclusive && base_raw_exclusive())) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(w.state, code + 8);
    if (sym == XKB_KEY_NoSymbol) return;
    int ctrl = w.mods & KEYS_CTRL, alt = w.mods & KEYS_ALT, meta = w.mods & KEYS_META, shift = w.mods & KEYS_SHIFT;
    int bare = !ctrl && !alt && !meta;
    int special = special_key(sym);
    if (special) {
        if (w.compose) xkb_compose_state_reset(w.compose);
        put_key(special, "", sym, code, is_repeat);
        return;
    }
    if (ctrl && !alt && !shift && sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
        put_key(K_CTRL_0 + (int)(sym - XKB_KEY_0), "", sym, code, is_repeat);
        return;
    }
    uint32_t c = xkb_keysym_to_utf32(sym);
    if (ctrl && (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '[' && c <= '_'))) {
        put_key((int)(c & 0x1F), "", sym, code, is_repeat);
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
        put_key(first, text, sym, code, is_repeat);
    } else if (first >= 0x80 && bare) {
        put_key(K_TEXT, text, sym, code, is_repeat);
    } else if (!first && w.name_keys && !modifier_sym(sym)) {
        put_key(K_NAMED, "", sym, code, is_repeat);
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
    if (base_raw_cancel) base_raw_cancel();
    stop_repeat();
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data; (void)kb; (void)surface; (void)keys;
    w.keyboard_serial = serial;
    w.keyboard_focused = 1;
    w.keyboard_focus_generation++;
}

static void keyboard_unfocus(void)
{
    w.keyboard_focused = 0;
    w.keyboard_focus_generation++;
    w.mods = 0;
    wl_backend_live_modifiers();
    if (base_raw_cancel) base_raw_cancel();
    stop_repeat();
    if (w.compose) xkb_compose_state_reset(w.compose);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
    keyboard_unfocus();
}

static int modifier_role(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: return KEYS_SHIFT;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: return KEYS_CTRL;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: return KEYS_ALT;
    case XKB_KEY_Meta_L: case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L: case XKB_KEY_Super_R: return KEYS_META;
    default: return 0;
    }
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t code,
                         uint32_t pressed)
{
    (void)data; (void)kb; (void)time;
    w.keyboard_serial = serial;
    if (base_raw_emit && w.state) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(w.state, code + 8);
        int down = pressed == WL_KEYBOARD_KEY_STATE_PRESSED;
        int local = 0;
        if (down && !(base_raw_exclusive && base_raw_exclusive())) {
            /* Modifiers arrive after the key; include the pressed key's
             * modifier role for local modifier-only bindings. */
            int held = w.mods | modifier_role(sym);
            int action = keys_match_action((int)code, !!(held & KEYS_CTRL), !!(held & KEYS_ALT),
                                           !!(held & KEYS_SHIFT), !!(held & KEYS_META));
            local = action == K_ACT_HOME || action == K_ACT_LOCK || action == K_ACT_SEARCH ||
                    (action >= K_VOL_UP && action <= K_BRI_DOWN);
        }
        raw_key(code, sym, (uint32_t)down, local);
        w.events++;
    }
    if (pressed != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (code == w.repeat_code) stop_repeat();
        purge_repeats(code);
        return;
    }
    key_pressed(code, 0);
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
    if (klen == 0) wl_backend_live_modifiers();
    if (w.keyboard_focused) w.keyboard_serial = serial;
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
    double px = wl_fixed_to_double(x);
    return (int)floor(w.logical_w > 0 ? px * w.pixel_w / w.logical_w : px);
}

static int picture_y(wl_fixed_t y)
{
    double py = wl_fixed_to_double(y);
    return (int)floor(w.logical_h > 0 ? py * w.pixel_h / w.logical_h : py);
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
    int base_size = size_text && atoi(size_text) > 0 && atoi(size_text) <= 128 ? atoi(size_text) : 24;
    int size = base_size * s;
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
    if (!image->width || !image->height) return;
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    if (!w.cursor_surface) w.cursor_surface = wl_compositor_create_surface(w.compositor);
    if (!w.cursor_surface) return;
    if (w.viewporter && !w.cursor_viewport)
        w.cursor_viewport = wp_viewporter_get_viewport(w.viewporter, w.cursor_surface);
    int d = 1, x, y;
    if (w.cursor_viewport) {
        wp_viewport_set_destination(w.cursor_viewport, base_size, base_size);
        x = (int)((uint64_t)image->hotspot_x * base_size / image->width);
        y = (int)((uint64_t)image->hotspot_y * base_size / image->height);
    } else {
        /* A theme's actual image need not be a multiple of its nominal scale. */
        d = s;
        while (image->width % d || image->height % d) d--;
        x = (int)(image->hotspot_x / d);
        y = (int)(image->hotspot_y / d);
    }
    wl_surface_set_buffer_scale(w.cursor_surface, d);
    wl_surface_attach(w.cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(w.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(w.cursor_surface);
    wl_pointer_set_cursor(w.pointer, w.enter_serial, w.cursor_surface, x, y);
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

static void pointer_unfocus(void)
{
    w.enter_serial = 0;
    memset(&w.axis, 0, sizeof w.axis);
    w.last_axis_source = w.active_axes = 0;
    pointer_window_leave();
    w.events++;
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)p; (void)serial; (void)surface;
    pointer_unfocus();
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
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        w.press_serial = serial;
        wl_data_pointer_pressed();
    }
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
    if (!w.axis.source) w.axis.source = w.last_axis_source;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
    (void)data; (void)p;
    w.axis.axes |= axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 1 : 2;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) w.axis.dv += discrete;
    else w.axis.dh += discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value120)
{
    (void)data; (void)p;
    w.axis.value120_axes |= axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 1 : 2;
    w.axis.axes |= axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 1 : 2;
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
    if (w.axis.axes) {
        int source = w.axis.source ? w.axis.source : SCROLL_WHEEL;
        double v = w.axis.v / SCROLL_PIXELS, h = w.axis.h / SCROLL_PIXELS;
        if (source == SCROLL_WHEEL) {
            if (w.axis.value120_axes & 1) v = w.axis.v120 / 120.0;
            else if (w.axis.dv) v = w.axis.dv;
            if (w.axis.value120_axes & 2) h = w.axis.h120 / 120.0;
            else if (w.axis.dh) h = w.axis.dh;
        }
        if (source != w.last_axis_source) w.active_axes = 0;
        w.active_axes = (w.active_axes & ~w.axis.axes) | (v != 0 ? 1 : 0) | (h != 0 ? 2 : 0);
        w.last_axis_source = w.active_axes ? source : SCROLL_NONE;
        pointer_window_scroll(source, v, h, w.axis.axes);
        w.events++;
    }
    memset(&w.axis, 0, sizeof w.axis);
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

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
                       struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)time;
    if (surface != w.surface) return;
    w.button_serial = serial;
    pointer_window_touch_down(id, picture_x(x), picture_y(y));
    w.events++;
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)time;
    pointer_window_touch_motion(id, picture_x(x), picture_y(y));
    w.events++;
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)data; (void)t; (void)time;
    w.button_serial = serial;
    pointer_window_touch_up(id);
    w.events++;
}

static void touch_frame(void *data, struct wl_touch *t)
{
    (void)data; (void)t;
}

static void touch_cancel(void *data, struct wl_touch *t)
{
    (void)data; (void)t;
    pointer_window_touch_cancel();
    w.events++;
}

static void touch_shape(void *data, struct wl_touch *t, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    (void)data; (void)t; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *t, int32_t id, wl_fixed_t orientation)
{
    (void)data; (void)t; (void)id; (void)orientation;
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down, .motion = touch_motion, .up = touch_up, .frame = touch_frame,
    .cancel = touch_cancel, .shape = touch_shape, .orientation = touch_orientation,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w.keyboard) {
        w.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w.keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && w.keyboard) {
        keyboard_unfocus();
        wl_keyboard_release(w.keyboard);
        w.keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w.pointer) {
        w.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w.pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && w.pointer) {
        pointer_unfocus();
        wl_pointer_release(w.pointer);
        w.pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !w.touch) {
        w.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(w.touch, &touch_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && w.touch) {
        touch_cancel(NULL, w.touch);
        if (wl_touch_get_version(w.touch) >= WL_TOUCH_RELEASE_SINCE_VERSION) wl_touch_release(w.touch);
        else wl_touch_destroy(w.touch);
        w.touch = NULL;
    }
    pointer_window_present(w.pointer || w.touch);
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
    int def_w = w.floating ? FLOAT_W : DEFAULT_W;
    int def_h = w.floating ? FLOAT_H : DEFAULT_H;
    int first_w = w.bounds_w > 0 && w.bounds_w < def_w ? w.bounds_w : def_w;
    int first_h = w.bounds_h > 0 && w.bounds_h < def_h ? w.bounds_h : def_h;
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
    if (w.scale != s) {
        w.scale = s;
        show_cursor();
    }
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
    put_key(K_CLOSE, "", XKB_KEY_NoSymbol, 0, 0);
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

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t width, uint32_t height)
{
    (void)data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    w.pending_w = (int)width;
    w.pending_h = (int)height;
    w.configured = 1;
    apply_size();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    (void)data; (void)ls;
    w.closed = 1;
    w.events++;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

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
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0 && !w.layer_shell) {
        w.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
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
    b->dirty_x0 = 0;
    b->dirty_y0 = 0;
    b->dirty_x1 = width;
    b->dirty_y1 = height;
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
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pw) x1 = pw;
    if (y1 > ph) y1 = ph;
    if (x0 >= x1 || y0 >= y1) {
        x0 = y0 = 0;
        x1 = pw;
        y1 = ph;
    }
    struct buffer *other = (b == &w.buffers[0]) ? &w.buffers[1] : &w.buffers[0];
    if (other->buffer && other->width == pw && other->height == ph) {
        if (other->dirty_x0 < other->dirty_x1 && other->dirty_y0 < other->dirty_y1) {
            if (x0 < other->dirty_x0) other->dirty_x0 = x0;
            if (y0 < other->dirty_y0) other->dirty_y0 = y0;
            if (x1 > other->dirty_x1) other->dirty_x1 = x1;
            if (y1 > other->dirty_y1) other->dirty_y1 = y1;
        } else {
            other->dirty_x0 = x0;
            other->dirty_y0 = y0;
            other->dirty_x1 = x1;
            other->dirty_y1 = y1;
        }
    } else if (other->buffer) {
        /* Another size now: its picture is copied in full if it is shown again at its own size. */
        other->dirty_x0 = other->dirty_y0 = 0;
        other->dirty_x1 = other->width;
        other->dirty_y1 = other->height;
    }
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
    if (b->dirty_x0 < b->dirty_x1 && b->dirty_y0 < b->dirty_y1) {
        if (b->dirty_x0 < cx0) cx0 = b->dirty_x0;
        if (b->dirty_y0 < cy0) cy0 = b->dirty_y0;
        if (b->dirty_x1 > cx1) cx1 = b->dirty_x1;
        if (b->dirty_y1 > cy1) cy1 = b->dirty_y1;
    }
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > pw) cx1 = pw;
    if (cy1 > ph) cy1 = ph;
    if (cx0 == 0 && cx1 == pw) {
        memcpy(b->pixels + (size_t)cy0 * (size_t)pw,
               pixels + (size_t)cy0 * (size_t)pw,
               (size_t)(cy1 - cy0) * (size_t)pw * 4);
    } else {
        size_t row_bytes = (size_t)(cx1 - cx0) * 4;
        for (int y = cy0; y < cy1; y++) {
            size_t off = (size_t)y * (size_t)pw + (size_t)cx0;
            memcpy(b->pixels + off, pixels + off, row_bytes);
        }
    }
    b->dirty_x0 = b->dirty_y0 = b->dirty_x1 = b->dirty_y1 = 0;
    b->busy = 1;
    wl_surface_attach(w.surface, b->buffer, 0, 0);
    if (w.viewport) {
        wp_viewport_set_destination(w.viewport, w.logical_w, w.logical_h);
    } else if (w.buffer_scale_set != w.integer_scale) {
        wl_surface_set_buffer_scale(w.surface, w.integer_scale);
        w.buffer_scale_set = w.integer_scale;
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

static void watch_theme(void)
{
    if (!theme_watch_open || !theme_watch_read || !theme_reload) return;
    w.theme_fd = theme_watch_open();
    theme_reload(); /* arm first so a concurrent save cannot be missed */
}

static void theme_event(void)
{
    if (w.theme_fd >= 0 && theme_watch_read && theme_watch_read(w.theme_fd)) {
        w.theme_dirty = 1;
        w.events++;
    }
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
    watch_theme();
    w.registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(w.registry, &registry_listener, NULL);
    const char *fl = getenv("LOCACHY_WINDOW_FLOATING");
    w.floating = fl && strcmp(fl, "1") == 0;
    if (wl_display_roundtrip(w.display) < 0 || !w.compositor || !w.shm ||
        (!w.wm_base && !(w.floating && w.layer_shell))) {
        say("window: the compositor lacks wl_compositor 4, wl_shm or shell");
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
    const char *title = getenv("LOCACHY_WINDOW_TITLE"), *app_id = getenv("LOCACHY_WINDOW_APP_ID");
    if (w.floating && w.layer_shell) {
        /* Centered always-on-top overlay on the focused output (anchor 0 = centered),
         * with exclusive keyboard focus so typing starts immediately without a click. */
        w.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            w.layer_shell, w.surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            app_id && *app_id ? app_id : "locachy");
        zwlr_layer_surface_v1_set_size(w.layer_surface, FLOAT_W, FLOAT_H);
        zwlr_layer_surface_v1_set_anchor(w.layer_surface, 0);
        zwlr_layer_surface_v1_set_exclusive_zone(w.layer_surface, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            w.layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        zwlr_layer_surface_v1_add_listener(w.layer_surface, &layer_surface_listener, NULL);
    } else {
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
            zxdg_toplevel_decoration_v1_set_mode(
                w.decoration, w.floating ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                         : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        xdg_toplevel_set_title(w.toplevel, title && *title ? title : "LOCachy");
        xdg_toplevel_set_app_id(w.toplevel, app_id && *app_id ? app_id : "locachy");
        xdg_toplevel_set_min_size(w.toplevel, 320, 200);
    }
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
    wl_backend_live_modifiers();
    if (!w.display || w.closed) return -1;
    if (count > 64) count = 64;
    if (w.frame_due) {
        w.frame_due = 0;
        w.held = 0;
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
    enum { DISPLAY, REPEAT, TOUCH, THEME, EXTRA };
    struct pollfd p[EXTRA + 64 + DATA_FDS];
    p[THEME] = (struct pollfd){ .fd = w.theme_fd, .events = POLLIN };
    p[DISPLAY] = (struct pollfd){ .fd = wl_display_get_fd(w.display), .events = POLLIN | out };
    p[REPEAT] = (struct pollfd){ .fd = w.repeat_fd, .events = POLLIN };
    p[TOUCH] = (struct pollfd){ .fd = pointer_window_touch_fd(), .events = POLLIN };
    for (size_t i = 0; i < count; i++) {
        p[EXTRA + i] = extra[i];
        p[EXTRA + i].revents = 0;
    }
    int ndata = wl_data_poll(p + EXTRA + count, DATA_FDS);
    if (ndata < 0) ndata = 0;
    if (ndata > DATA_FDS) ndata = DATA_FDS;
    int timeout = timeout_ms, data_timeout = wl_data_timeout(), shortened = 0;
    if (data_timeout >= 0 && (timeout < 0 || data_timeout < timeout)) {
        timeout = data_timeout;
        shortened = 1;
    }
    int r = poll(p, (nfds_t)(EXTRA + count + (size_t)ndata), timeout);
    if (r < 0) {
        int error = errno;
        wl_display_cancel_read(w.display);
        if (error == EINTR) {
            errno = EINTR;
            return 2;
        }
        return -1;
    }
    if (p[DISPLAY].revents & (POLLIN | POLLERR | POLLHUP)) {
        if (wl_display_read_events(w.display) < 0) return -1;
    } else {
        wl_display_cancel_read(w.display);
    }
    if (wl_display_dispatch_pending(w.display) < 0) return -1;
    if (p[REPEAT].revents & POLLIN) {
        uint64_t expired = 0;
        if (read(w.repeat_fd, &expired, sizeof expired) == (ssize_t)sizeof expired && expired > 0 && w.repeat_code) {
            if (base_raw_emit && w.state)
                raw_key(w.repeat_code, xkb_state_key_get_one_sym(w.state, w.repeat_code + 8), 2, 0);
            key_pressed(w.repeat_code, 1);
        }
    }
    if (p[TOUCH].revents & POLLIN) pointer_window_touch_timer();
    if (p[THEME].revents & POLLIN) theme_event();
    if (p[THEME].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(w.theme_fd);
        w.theme_fd = -1;
    }
    if (ndata > 0) wl_data_dispatch(p + EXTRA + count, (size_t)ndata);
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        extra[i].revents = p[EXTRA + i].revents;
        ready |= extra[i].revents != 0;
    }
    if (w.closed) return -1;
    if (w.frame_due) {
        w.frame_due = 0;
        w.held = 0;
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
    if (w.cursor_viewport) wp_viewport_destroy(w.cursor_viewport);
    if (w.cursor_surface) wl_surface_destroy(w.cursor_surface);
    if (w.cursor_theme) wl_cursor_theme_destroy(w.cursor_theme);
    if (w.pointer) {
        pointer_unfocus();
        wl_pointer_release(w.pointer);
    }
    if (w.touch) {
        pointer_window_touch_cancel();
        if (wl_touch_get_version(w.touch) >= WL_TOUCH_RELEASE_SINCE_VERSION) wl_touch_release(w.touch);
        else wl_touch_destroy(w.touch);
    }
    pointer_window_present(0);
    if (w.keyboard) wl_keyboard_release(w.keyboard);
    if (w.layer_surface) zwlr_layer_surface_v1_destroy(w.layer_surface);
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
    if (w.layer_shell) {
        if (zwlr_layer_shell_v1_get_version(w.layer_shell) >= ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
            zwlr_layer_shell_v1_destroy(w.layer_shell);
        else wl_proxy_destroy((struct wl_proxy *)w.layer_shell);
    }
    if (w.wm_base) xdg_wm_base_destroy(w.wm_base);
    if (w.shm) wl_shm_destroy(w.shm);
    if (w.compositor) wl_compositor_destroy(w.compositor);
    if (w.registry) wl_registry_destroy(w.registry);
    if (w.display) wl_display_disconnect(w.display);
    if (w.repeat_fd >= 0) close(w.repeat_fd);
    if (w.theme_fd >= 0) close(w.theme_fd);
    xkb_compose_state_unref(w.compose);
    xkb_compose_table_unref(w.compose_table);
    xkb_state_unref(w.state);
    xkb_keymap_unref(w.keymap);
    xkb_context_unref(w.xkb);
    memset(&w, 0, sizeof w);
    w.repeat_fd = -1;
    w.theme_fd = -1;
    w.integer_scale = 1;
    w.cursor_shape = CURSOR_ARROW;
    khead = klen = 0;
}
#else
/* Direct-only builds: no window transport (screen.c refuses a request). */
typedef int wayland_backend_not_built;
#endif
