/* The window transport for the software canvas (wayland-backend.c): an
 * explicit choice of the trusted window launcher, LOCACHY_BACKEND=wayland.
 * Without it every program stays direct. Direct-only builds (process 1's
 * musl build, programs built without CONFIG_WAYLAND) compile none of it; an
 * explicit request that cannot be met ends the program, never opening DRM or
 * input devices instead. Inside the base only; programs see base.h. */
#ifndef LOCACHY_WAYLAND_BACKEND_H
#define LOCACHY_WAYLAND_BACKEND_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 0 direct (unset, empty or "direct"), 1 wayland, -1 any other value. */
static inline int wl_backend_requested(void)
{
    const char *mode = getenv("LOCACHY_BACKEND");
    if (!mode || !*mode || strcmp(mode, "direct") == 0) return 0;
    return strcmp(mode, "wayland") == 0 ? 1 : -1;
}

/* Exit statuses of a window request that cannot be met (sysexits.h's). */
#define WL_EXIT_CONFIG 78      /* an unknown backend, or not built in */
#define WL_EXIT_UNAVAILABLE 69 /* no compositor, or no window from it */

#ifdef CONFIG_WAYLAND
struct wl_display;
struct wl_seat;
struct wl_surface;
/* Connect and wait for the window's first configure. resize(w, h, scale)
 * gets the buffer size in pixels and the preferred scale, for each change;
 * frame() is called when a picture held back by an unfinished frame can be
 * shown. 0, or -1 (all undone). */
int wl_backend_open(int (*resize)(int width, int height, double scale), void (*frame)(void));
int wl_backend_active(void);
void wl_backend_close(void);
/* Show the picture (w x h, XRGB), changed in [x0, x1) x [y0, y1): 0 shown,
 * 1 held back until the compositor finishes the frame it shows (frame()),
 * -1 failed. The first picture shown tells the launcher the window is ready. */
int wl_backend_flush(const uint32_t *pixels, int w, int h, int x0, int y0, int x1, int y1);
/* Wait at most timeout_ms (-1: no limit) for window events or extra fds,
 * which keep their events and receive revents. 1 window events handled, 2
 * an extra fd is ready, 0 the time passed, -1 the window was closed or the
 * compositor has gone. */
int wl_backend_wait(struct pollfd *extra, size_t count, int timeout_ms);
/* The next key typed, as base.h's codes (0: none), with the text it typed
 * (K_TEXT's) and its keysym name; the modifiers held. */
int wl_backend_next_key(void);
const char *wl_backend_text(void);
const char *wl_backend_key_name(void);
int wl_backend_modifiers(void);
/* While on, a key that types nothing (Henkan_Mode) comes as K_NAMED (keys_name_any). */
void wl_backend_name_keys(int on);
/* A close request is K_CLOSE (base.h); after reading it the program stays
 * open on purpose (key_close_kept). wl_backend_wait's -1 after a second close
 * within 3 s of an unanswered K_CLOSE: wl_backend_close_forced() is 1 (else
 * the compositor has gone). */
void wl_backend_close_kept(void);
int wl_backend_close_forced(void);
/* The frame the compositor draws: 0 not answered, 1 none (client side), 2 its own. */
int wl_backend_decoration(void);
void wl_backend_cursor(int shape);
/* For the data device (wayland-data.c): the connection, the seat, the
 * window's surface, and the serials of the last key and button events. */
struct wl_display *wl_backend_display(void);
struct wl_seat *wl_backend_seat(void);
struct wl_surface *wl_backend_surface(void);
uint32_t wl_backend_keyboard_serial(void);
uint32_t wl_backend_button_serial(void);

/* The base's side, for the transport: a new size is told as K_SHOWN once
 * (screen.c); pointer events become the pointer's own (pointer.c), in
 * pixels of the picture; scrolling in wheel steps, positive down. */
int screen_window_shown(void);
/* The pixel rows of the canvas above the window (the hidden top bar's). */
int screen_window_top(void);
void pointer_window_motion(int x, int y);
void pointer_window_button(int code, int pressed);
void pointer_window_scroll(int source, double v, double h, int axes);
void pointer_window_present(int on);
void pointer_window_leave(void);
#else
static inline int wl_backend_active(void) { return 0; }
#endif
#endif
