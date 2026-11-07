/* Standard Wayland selections and drag source for windows of the shared
 * software canvas. No host clipboard socket or compositor-private protocol.
 * Callers outside wayland-data.c see weak declarations: test or static
 * binaries without it leave them NULL, so check a function before calling. */
#ifndef LOCACHY_WAYLAND_DATA_H
#define LOCACHY_WAYLAND_DATA_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#ifdef WL_DATA_IMPLEMENTATION
#define WL_DATA_WEAK
#else
#define WL_DATA_WEAK __attribute__((weak))
#endif

struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_surface;
WL_DATA_WEAK void wl_data_global(struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
WL_DATA_WEAK void wl_data_init(struct wl_display *display, struct wl_seat *seat, struct wl_surface *surface);
WL_DATA_WEAK void wl_data_close(void);
/* Pending writes to requesters: appended to the caller's poll set (the count
 * is returned), dispatched with that same segment, ended at the deadline. */
WL_DATA_WEAK int wl_data_poll(struct pollfd *out, size_t max);
WL_DATA_WEAK void wl_data_dispatch(const struct pollfd *fds, size_t count);
WL_DATA_WEAK int wl_data_timeout(void);
/* 1 once a data device exists: clipboard.c then uses these instead of the
 * host clipboard service. Status values are clipboard.h's. A get returns a
 * sealed memfd with the exact size, as the host service does. */
WL_DATA_WEAK int wl_data_active(void);
WL_DATA_WEAK int wl_data_clipboard_get(int primary, const char *mime, int *fd, size_t *size);
WL_DATA_WEAK int wl_data_clipboard_set(int primary, const char *mime, const void *data, size_t size);
WL_DATA_WEAK int wl_data_clipboard_clear(int primary);
/* The serial of the last pointer button event (the press of a held button
 * while a drag is in progress), from the window backend; 0 if none. */
WL_DATA_WEAK uint32_t wl_data_press_serial(void);
/* A text/uri-list drag, copy only. A URI is not a document-portal grant: the
 * caller offers only paths the recipient can already read. 0: requested. */
WL_DATA_WEAK int wl_data_drag_start(const char *uris, size_t size, uint32_t press_serial);
/* The same with a document-portal file transfer key (a string), offered as
 * application/vnd.portal.filetransfer and application/vnd.portal.files
 * (tools/DOCUMENTS.md). ended(finished, data) is called once, from the
 * event dispatch, when the drag ends: 1 when a target read the drop, 0 when
 * it was cancelled or the data device went. Not without data device
 * version 3, which tells the end. 0: requested (ended will be called). */
WL_DATA_WEAK int wl_data_drag_start_key(const char *uris, size_t size, const char *key, uint32_t press_serial,
                                        void (*ended)(int finished, void *data), void *data);
WL_DATA_WEAK int wl_data_drag_active(void);
/* 1 when a target finished reading the last drag; 0 if cancelled or open. */
WL_DATA_WEAK int wl_data_drag_finished(void);
#endif
