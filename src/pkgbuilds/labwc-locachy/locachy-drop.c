/* SPDX-License-Identifier: GPL-2.0-only
 * The drop gate of private-file drags. A host source that offers
 * application/vnd.portal.filetransfer carries documents no client may read
 * yet. The document service grants them to the drop target alone, and only
 * the compositor knows that target before the drop is delivered.
 *
 * The release that would deliver such a drop is held while the service is
 * asked on its control socket (SOCK_SEQPACKET, one request per connection):
 *   drop SOURCE_PID container NAME | drop SOURCE_PID host | drop SOURCE_PID xwayland
 * "ok" forwards the release and wlroots delivers the drop. "refuse WHY", no
 * answer within DROP_TIMEOUT_MS, no service, or another drop target by then
 * cancel the drag as wlroots does (its source is destroyed); the release is
 * forwarded after that. Connecting, sending and the answer never block the
 * loop. Other sources, other MIME types and drops wlroots would not deliver
 * anyway go to the seat as they are.
 */
#define _GNU_SOURCE
#include "locachy-drop.h"
#include "labwc.h"
#include "locachy-clipboard.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>
#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#define PORTAL_FILES "application/vnd.portal.filetransfer"
#define DOCS_CONTROL "/run/locachy/docs/control"
#define DROP_TIMEOUT_MS 300

enum client_kind { KIND_UNKNOWN, KIND_HOST, KIND_CONTAINER, KIND_XWAYLAND };

static struct {
    bool active, touch;
    struct wlr_seat *seat;
    struct wlr_drag *drag;
    struct wl_client *target;
    uint32_t time_msec, button;
    int32_t touch_id;
    int fd;
    struct wl_event_source *answer, *timer;
    struct wl_listener drag_destroy, target_destroy;
} held = { .fd = -1 };

static bool name_char(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        (!first && (c == '_' || c == '.' || c == '-'));
}

/* The outermost app_NAME group of a client's cgroup. A container may nest
 * groups below the one its launcher made, never rename that one. A native
 * window's group, app_window-PROGRAM-N, names no container: process 1 runs
 * such a program (files, view, settings) as root on the host's own socket,
 * a host client as a source and as a target. */
static bool container_name(int cgroup, char *name, size_t size)
{
    static const char top[] = "/sys/fs/cgroup/";
    char link[64], path[4096];
    if (cgroup < 0) return false;
    snprintf(link, sizeof link, "/proc/self/fd/%d", cgroup);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return false;
    path[n] = 0;
    if (strncmp(path, top, sizeof top - 1) != 0) return false;
    for (char *part = path + sizeof top - 1; part; ) {
        char *next = strchr(part, '/');
        if (next) *next++ = 0;
        if (strncmp(part, "app_", 4) == 0) {
            const char *s = part + 4;
            if (strncmp(s, "window-", 7) == 0) return false;
            size_t length = strlen(s);
            if (length == 0 || length > 64 || length >= size) return false;
            for (size_t i = 0; i < length; i++) {
                if (!name_char(s[i], i == 0)) return false;
            }
            memcpy(name, s, length + 1);
            return true;
        }
        part = next;
    }
    return false;
}

static enum client_kind client_kind(struct wl_client *client, char *name, size_t size)
{
#if HAVE_XWAYLAND
    if (server.xwayland && server.xwayland->server && client == server.xwayland->server->client) {
        return KIND_XWAYLAND;
    }
#endif
    if (container_name(locachy_client_cgroup(client), name, size)) return KIND_CONTAINER;
    /* A security context outside every container group cannot be named. */
    return wlr_security_context_manager_v1_lookup_client(server.security_context_manager_v1, client)
        ? KIND_UNKNOWN : KIND_HOST;
}

static bool offers_portal_files(struct wlr_data_source *source)
{
    char **type;
    wl_array_for_each(type, &source->mime_types) {
        if (strcmp(*type, PORTAL_FILES) == 0) return true;
    }
    return false;
}

static void forward(void)
{
    if (held.touch) {
        wlr_seat_touch_notify_up(held.seat, held.time_msec, held.touch_id);
    } else {
        wlr_seat_pointer_notify_button(held.seat, held.time_msec, held.button,
            WL_POINTER_BUTTON_STATE_RELEASED);
        wlr_seat_pointer_notify_frame(held.seat);
    }
}

static void release_wait(void)
{
    if (held.answer) wl_event_source_remove(held.answer);
    if (held.timer) wl_event_source_remove(held.timer);
    held.answer = held.timer = NULL;
    if (held.fd >= 0) close(held.fd);
    held.fd = -1;
    if (held.target) wl_list_remove(&held.target_destroy.link);
    if (held.drag) wl_list_remove(&held.drag_destroy.link);
    held.target = NULL;
    held.drag = NULL;
}

/* The drop goes ahead only if granted and still aimed at the granted client. */
static void settle(bool granted)
{
    if (!held.active) return;
    held.active = false;
    struct wlr_drag *drag = held.drag;
    bool same = drag && held.target && drag->focus_client && drag->focus_client->client == held.target;
    release_wait();
    if (drag && drag->source && !(granted && same)) {
        if (granted) wlr_log(WLR_INFO, "LOCachy drop gate: the drop target changed; drag cancelled");
        wlr_data_source_destroy(drag->source);
    }
    forward();
}

static int handle_answer(int fd, uint32_t mask, void *data)
{
    (void)data;
    char text[128];
    ssize_t n = recv(fd, text, sizeof text - 1, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
        return 0;
    }
    bool ok = (n == 2 || (n == 3 && text[2] == '\n')) && memcmp(text, "ok", 2) == 0;
    if (!ok) {
        if (n > 0) {
            text[n] = 0;
            text[strcspn(text, "\n")] = 0;
        }
        wlr_log(WLR_INFO, "LOCachy drop gate: %s; drag cancelled",
            n > 0 ? text : "the document service closed");
    }
    settle(ok);
    return 0;
}

static int handle_timeout(void *data)
{
    (void)data;
    if (held.drag && held.target) {
        wlr_log(WLR_ERROR, "LOCachy drop gate: no answer in %d ms; drag cancelled", DROP_TIMEOUT_MS);
    } else if (held.drag) {
        wlr_log(WLR_INFO, "LOCachy drop gate: the drop target disconnected; drag cancelled");
    }
    settle(false);
    return 0;
}

/* Nothing is left to deliver: forward the release on the next dispatch. */
static void handle_drag_destroy(struct wl_listener *listener, void *data)
{
    (void)listener; (void)data;
    wl_list_remove(&held.drag_destroy.link);
    held.drag = NULL;
    wl_event_source_timer_update(held.timer, 1);
}

static void handle_target_destroy(struct wl_listener *listener, void *data)
{
    (void)listener; (void)data;
    wl_list_remove(&held.target_destroy.link);
    held.target = NULL;
    wl_event_source_timer_update(held.timer, 1);
}

/* Ask about the drop this release would deliver; true while it is held. */
static bool hold(struct wlr_seat *seat)
{
    struct wlr_drag *drag = seat->drag;
    struct wlr_data_source *source = drag->source;
    /* wlroots cancels a drop that no client accepted by itself. */
    if (!source || !drag->focus_client || !source->accepted || !source->current_dnd_action ||
            !drag->seat_client || !offers_portal_files(source)) {
        return false;
    }
    char name[65], request[128];
    if (client_kind(drag->seat_client->client, name, sizeof name) != KIND_HOST) return false;
    pid_t pid = 0;
    wl_client_get_credentials(drag->seat_client->client, &pid, NULL, NULL);
    struct wl_client *target = drag->focus_client->client;
    switch (client_kind(target, name, sizeof name)) {
    case KIND_CONTAINER:
        snprintf(request, sizeof request, "drop %ld container %s", (long)pid, name);
        break;
    case KIND_HOST:
        snprintf(request, sizeof request, "drop %ld host", (long)pid);
        break;
    case KIND_XWAYLAND:
        snprintf(request, sizeof request, "drop %ld xwayland", (long)pid);
        break;
    default:
        wlr_log(WLR_ERROR, "LOCachy drop gate: the drop target cannot be attributed; drag cancelled");
        wlr_data_source_destroy(source);
        return false;
    }
    size_t length = strlen(request);
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
            send(fd, request, length, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)length) {
        wlr_log_errno(WLR_ERROR, "LOCachy drop gate: no document service; drag cancelled");
        if (fd >= 0) close(fd);
        wlr_data_source_destroy(source);
        return false;
    }
    held.fd = fd;
    held.answer = wl_event_loop_add_fd(server.wl_event_loop, fd, WL_EVENT_READABLE, handle_answer, NULL);
    held.timer = wl_event_loop_add_timer(server.wl_event_loop, handle_timeout, NULL);
    if (!held.answer || !held.timer) {
        release_wait();
        wlr_data_source_destroy(source);
        return false;
    }
    wl_event_source_timer_update(held.timer, DROP_TIMEOUT_MS);
    held.seat = seat;
    held.drag = drag;
    held.drag_destroy.notify = handle_drag_destroy;
    wl_signal_add(&drag->events.destroy, &held.drag_destroy);
    held.target = target;
    held.target_destroy.notify = handle_target_destroy;
    wl_client_add_destroy_listener(target, &held.target_destroy);
    held.active = true;
    return true;
}

/* Whether this release ends the drag button's last press: only that one
 * reaches the drag's grab, as wlroots counts presses. */
static bool ends_drag_press(struct wlr_seat *seat, uint32_t button)
{
    struct wlr_seat_pointer_state *state = &seat->pointer_state;
    if (!seat->drag || seat->drag->grab_type != WLR_DRAG_GRAB_KEYBOARD_POINTER ||
            state->grab_button != button) {
        return false;
    }
    for (size_t i = 0; i < state->button_count; i++) {
        if (state->buttons[i].button == button) return state->buttons[i].n_pressed == 1;
    }
    return false;
}

/* An event after a held release must not overtake it: that one is cancelled. */
static void settle_before_next(void)
{
    if (!held.active) return;
    wlr_log(WLR_INFO, "LOCachy drop gate: input during the wait; drag cancelled");
    settle(false);
}

void locachy_drop_pointer_button(struct wlr_seat *seat, uint32_t time_msec,
    uint32_t button, enum wl_pointer_button_state state)
{
    settle_before_next();
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && ends_drag_press(seat, button) && hold(seat)) {
        held.touch = false;
        held.time_msec = time_msec;
        held.button = button;
        return;
    }
    wlr_seat_pointer_notify_button(seat, time_msec, button, state);
}

void locachy_drop_touch_up(struct wlr_seat *seat, uint32_t time_msec, int32_t touch_id)
{
    settle_before_next();
    if (seat->drag && seat->drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_TOUCH &&
            seat->drag->grab_touch_id == touch_id && hold(seat)) {
        held.touch = true;
        held.time_msec = time_msec;
        held.touch_id = touch_id;
        return;
    }
    wlr_seat_touch_notify_up(seat, time_msec, touch_id);
}

/* Escape cancels an active drag, as mutter and kwin do; the drag's own
 * keyboard grab would only swallow the key. The cancel is wlroots' own: the
 * source is destroyed (it gets "cancelled", the target only "leave"), and a
 * drag without a source ends with its grab. A release still held for the
 * document service then goes on without a drop. */
bool locachy_drop_escape(struct wlr_seat *seat, const xkb_keysym_t *syms, int count)
{
    struct wlr_drag *drag = seat->drag;
    if (!drag) return false;
    int i = 0;
    while (i < count && syms[i] != XKB_KEY_Escape) i++;
    if (i == count) return false;
    wlr_log(WLR_INFO, "LOCachy drop gate: Escape; drag cancelled");
    if (drag->source) {
        wlr_data_source_destroy(drag->source);
    } else if (drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_POINTER) {
        wlr_seat_pointer_end_grab(seat);
    } else if (drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_TOUCH) {
        wlr_seat_touch_end_grab(seat);
    } else {
        wlr_seat_keyboard_end_grab(seat);
    }
    return true;
}

void locachy_drop_finish(void)
{
    held.active = false;
    release_wait();
}
