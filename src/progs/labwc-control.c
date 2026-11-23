/*
 * labwc-control (/usr/local/lib/locachy/labwc-control): the host's trusted
 * client of the shared compositor. locachy-seat starts it once labwc is up.
 *
 * Containers' display: $XDG_RUNTIME_DIR/locachy-0, a wp_security_context_v1
 * listener. labwc hides its privileged interfaces (screen capture, virtual
 * keyboard, window and output management, layer shell, ...) from every
 * client that connects there; wayland-0 is for host programs only.
 *
 * $XDG_RUNTIME_DIR/locachy-control.sock (root only, SOCK_SEQPACKET), one
 * request per connection and one reply:
 *   show NAME        unminimize and activate the windows of container NAME:
 *                    "ok shown N"; with none open, locachy-bar shows the
 *                    program from its tray item, or asks whether to start
 *                    its program again ("ok shown 0, ..."), or "error ..."
 *   raise NAME       the same, without locachy-bar: for a window that is
 *                    still coming (a file just handed to the program)
 *   close NAME       ask the windows of container NAME to close cleanly:
 *                    "ok closed N" (0 when none is open)
 *   reload-monitors  apply MONITORS_CONF: "ok monitors" or "error ..."
 *   focused          the container of the focused window: "ok NAME", or "ok"
 *                    when no container's window has the focus
 *   home, lock,      (labwc-control --action WHAT, from the touchpad's swipes)
 *   next, prev       asked of process 1 through locachy-seat: "ok WHAT"
 * A window belongs to container NAME when its app_id names a .desktop file
 * that NAME installed (or that file's StartupWMClass), the way desktop shells
 * match windows to applications, or its command (a toolkit's app id when a
 * program sets none); a window whose app_id no container has is its parent
 * window's (wlr-foreign-toplevel's parent: a dialog another program draws
 * for it, as a portal's file chooser).
 *
 * Screens: the exact connected display set selects a saved profile;
 * MONITORS_CONF remains the unchanged legacy fallback. Selection runs at
 * start, on file changes, and when a head or its enabled/mode capabilities
 * change. A proposed configuration is tested before apply; refusal keeps
 * the active layout instead of repeating the same failing request. Modes
 * may include exact millihertz (WIDTHxHEIGHT@MILLIHZ); scale and signed
 * logical coordinates are restored together.
 *
 * Idleness: labwc's inhibitable notifications separately time automatic
 * locking and display-off. The private link to locachy-seat forwards idle
 * events to process 1 and resets the timers on a seat handover. Display-off
 * uses retained output-power objects to restore only outputs switched off here.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "security-context-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "base.h"
#include "monitor-layout.h"
#include "monitor-profile.h"

#define MAX_IDS 16
#define BAR_SOCKET "locachy-bar.sock"

static struct wl_display *display;
static struct wl_seat *seat;
static struct wp_security_context_manager_v1 *security;
static struct zwlr_foreign_toplevel_manager_v1 *toplevels;
static struct zwlr_output_manager_v1 *outputs;
static uint32_t output_version;
static struct ext_idle_notifier_v1 *idle_notifier;
static struct ext_idle_notification_v1 *idle, *blank_idle;
static int idle_minutes = -1, blank_minutes = -1;
static int seat_active = 1;
/* Private, bidirectional link to locachy-seat (LOCACHY_SEAT_REQUESTS). */
static int seat_requests = -1;
static char container_path[108], control_path[108], bar_path[108];

static int idle_report(const char *event)
{
    size_t len = strlen(event);
    return seat_requests >= 0 &&
           send(seat_requests, event, len, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)len;
}

/* Windows */

struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct zwlr_foreign_toplevel_handle_v1 *parent; /* the window it is a dialog of */
    char app_id[128];
    int minimized, activated, announced;
    struct window *next;
};

static struct window *windows;

static void window_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    (void)data;
    (void)h;
    (void)title;
}

static void window_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
    struct window *w = data;
    (void)h;
    snprintf(w->app_id, sizeof w->app_id, "%s", id);
}

static void window_output(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                          struct wl_output *output)
{
    (void)data;
    (void)h;
    (void)output;
}

static void window_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                         struct wl_array *states)
{
    struct window *w = data;
    uint32_t *state;
    (void)h;
    w->minimized = w->activated = 0;
    wl_array_for_each(state, states) {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
            w->minimized = 1;
        } else if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            w->activated = 1;
        }
    }
}

static void window_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct window *w = data;
    (void)h;
    if (!w->announced && w->app_id[0]) {
        w->announced = 1;
        say("labwc-control: window opened, app_id %s", w->app_id);
    }
}

static void window_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct window *w = data;
    for (struct window **p = &windows; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            break;
        }
    }
    for (struct window *o = windows; o; o = o->next) {
        if (o->parent == h) {
            o->parent = NULL; /* its handle is gone with it */
        }
    }
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(w);
}

static void window_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                          struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    struct window *w = data;
    (void)h;
    w->parent = parent;
}

static const struct window *parent_of(const struct window *w)
{
    for (const struct window *o = windows; o && w->parent; o = o->next) {
        if (o->handle == w->parent) {
            return o;
        }
    }
    return NULL;
}

/* Does an app id of container name (open_window_ids) name window w's? */
static int named_by(const struct window *w, const char *name)
{
    char ids[MAX_IDS][OPEN_WINDOW_ID];
    int nids = open_window_ids(name, ids, MAX_IDS);
    for (int i = 0; i < nids; i++) {
        if (strcasecmp(w->app_id, ids[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The container of window w: the first whose app ids name its own, else its
 * parent window's (a dialog another program draws for it, as a portal's
 * file chooser); "" when none. */
static void window_container(const struct window *w, char *name, size_t size)
{
    char names[64][OPEN_NAME];
    int n = open_containers(names, 64);
    name[0] = '\0';
    for (int depth = 0; w && depth < 8; depth++, w = parent_of(w)) {
        for (int k = 0; k < n; k++) {
            if (named_by(w, names[k])) {
                snprintf(name, size, "%.63s", names[k]);
                return;
            }
        }
    }
}

/* Is window w container name's: its app id one of name's, or (an app id no
 * container has) its parent window name's? */
static int window_of(const struct window *w, const char *name)
{
    char owner[OPEN_NAME];
    if (named_by(w, name)) {
        return 1;
    }
    window_container(w, owner, sizeof owner);
    return strcmp(owner, name) == 0;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener window_listener = {
    .title = window_title,
    .app_id = window_app_id,
    .output_enter = window_output,
    .output_leave = window_output,
    .state = window_state,
    .done = window_done,
    .closed = window_closed,
    .parent = window_parent,
};

static void new_window(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                       struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)m;
    struct window *w = calloc(1, sizeof *w);
    if (!w) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    w->handle = handle;
    w->next = windows;
    windows = w;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &window_listener, w);
}

static void windows_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m)
{
    (void)data;
    zwlr_foreign_toplevel_manager_v1_destroy(m);
    toplevels = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener windows_listener = {
    .toplevel = new_window,
    .finished = windows_finished,
};

static int valid_name(const char *name)
{
    if (!isalnum((unsigned char)name[0]) || strlen(name) > 63) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return 1;
}

/* With no window of container name: locachy-bar, "ok WHAT" (the tray item
 * was asked for the window, or the user is asked whether to start the
 * program again); -1 if it cannot help. */
static int ask_bar(const char *name, char *reply, size_t size)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    char request[96], answer[128];
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), n;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", bar_path);
    n = snprintf(request, sizeof request, "show %.63s", name);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        send(fd, request, (size_t)n, MSG_NOSIGNAL) != n) {
        if (fd >= 0) close(fd);
        return -1;
    }
    struct pollfd wait = { .fd = fd, .events = POLLIN };
    ssize_t got = poll(&wait, 1, 100) > 0 ? recv(fd, answer, sizeof answer - 1, 0) : -1;
    close(fd);
    if (got <= 3) {
        return -1;
    }
    answer[got] = '\0';
    if (strncmp(answer, "ok ", 3) != 0) {
        return -1;
    }
    snprintf(reply, size, "ok shown 0, %s", answer + 3);
    return 0;
}

static void show(const char *name, char *reply, size_t size, int bar)
{
    char ids[MAX_IDS][OPEN_WINDOW_ID];
    if (!valid_name(name)) {
        snprintf(reply, size, "error invalid container name");
        return;
    }
    if (!toplevels || !seat) {
        snprintf(reply, size, "error the compositor offers no window control");
        return;
    }
    /* A managed host window (process 1's window-PROGRAM-N) has its own name
     * as app id and no .desktop file of a container. */
    int native = strncmp(name, "window-", 7) == 0;
    int nids = native ? 1 : open_window_ids(name, ids, MAX_IDS);
    int shown = 0;
    for (struct window *w = windows; w; w = w->next) {
        if (native ? strcmp(w->app_id, name) == 0 : window_of(w, name)) {
            if (w->minimized) {
                zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
            }
            zwlr_foreign_toplevel_handle_v1_activate(w->handle, seat);
            shown++;
        }
    }
    wl_display_flush(display);
    if (shown) {
        snprintf(reply, size, "ok shown %d", shown);
    } else if (!bar || ask_bar(name, reply, size) < 0) {
        snprintf(reply, size, nids ? "error no window of %.63s is open" :
                 "error %.63s has no .desktop file to match its windows", name);
    }
}

static void close_windows(const char *name, char *reply, size_t size)
{
    if (!valid_name(name)) {
        snprintf(reply, size, "error invalid container name");
        return;
    }
    if (!toplevels) {
        snprintf(reply, size, "ok closed 0");
        return;
    }
    int closed = 0;
    for (struct window *w = windows; w; w = w->next) {
        if (window_of(w, name)) {
            zwlr_foreign_toplevel_handle_v1_close(w->handle);
            closed++;
        }
    }
    wl_display_flush(display);
    snprintf(reply, size, "ok closed %d", closed);
}

static void focused(char *reply, size_t size)
{
    struct window *w = windows;
    while (w && !w->activated) {
        w = w->next;
    }
    char name[OPEN_NAME] = "";
    if (w) {
        window_container(w, name, sizeof name);
    }
    snprintf(reply, size, name[0] ? "ok %.63s" : "ok", name);
}

/* Screens */

struct mode {
    struct zwlr_output_mode_v1 *mode;
    struct head *head; /* NULL once the head has gone */
    int width, height, refresh, preferred;
    struct mode *next;
};

struct head {
    struct zwlr_output_head_v1 *head;
    char name[64];
    int enabled, x, y;
    double scale;
    struct mode *modes, *current;
    struct head *next;
};

static struct head *heads;
static uint32_t output_serial;
static int heads_changed, reload_wanted;
static double apply_at; /* apply MONITORS_CONF then (uptime), if not 0 */
static enum { POWER_AWAKE, POWER_PROBING, POWER_BLANKING, POWER_DARK, POWER_WAKING } power_phase;

static void mode_size(void *data, struct zwlr_output_mode_v1 *m, int32_t width, int32_t height)
{
    struct mode *mode = data;
    (void)m;
    if (mode->width != width || mode->height != height) heads_changed = 1;
    mode->width = width;
    mode->height = height;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *m, int32_t refresh)
{
    struct mode *mode = data;
    (void)m;
    if (mode->refresh != refresh) heads_changed = 1;
    mode->refresh = refresh;
}

static void mode_preferred(void *data, struct zwlr_output_mode_v1 *m)
{
    struct mode *mode = data;
    (void)m;
    mode->preferred = 1;
}

static void mode_finished(void *data, struct zwlr_output_mode_v1 *m)
{
    struct mode *mode = data;
    struct head *h = mode->head;
    heads_changed = 1;
    for (struct mode **p = h ? &h->modes : NULL; p && *p; p = &(*p)->next) {
        if (*p == mode) {
            *p = mode->next;
            if (h->current == mode) {
                h->current = NULL;
            }
            break;
        }
    }
    if (output_version >= 3) {
        zwlr_output_mode_v1_release(m);
    } else {
        zwlr_output_mode_v1_destroy(m);
    }
    free(mode);
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size,
    .refresh = mode_refresh,
    .preferred = mode_preferred,
    .finished = mode_finished,
};

static void head_name(void *data, struct zwlr_output_head_v1 *hd, const char *name)
{
    struct head *h = data;
    (void)hd;
    if (strcmp(h->name, name)) heads_changed = 1;
    snprintf(h->name, sizeof h->name, "%s", name);
}

static void head_string(void *data, struct zwlr_output_head_v1 *hd, const char *s)
{
    (void)data;
    (void)hd;
    (void)s;
}

static void head_physical_size(void *data, struct zwlr_output_head_v1 *hd, int32_t w, int32_t h)
{
    (void)data;
    (void)hd;
    (void)w;
    (void)h;
}

static void head_mode(void *data, struct zwlr_output_head_v1 *hd, struct zwlr_output_mode_v1 *m)
{
    struct head *h = data;
    (void)hd;
    struct mode *mode = calloc(1, sizeof *mode);
    if (!mode) {
        return;
    }
    mode->mode = m;
    mode->head = h;
    mode->next = h->modes;
    h->modes = mode;
    zwlr_output_mode_v1_add_listener(m, &mode_listener, mode);
}

static void head_enabled(void *data, struct zwlr_output_head_v1 *hd, int32_t enabled)
{
    struct head *h = data;
    (void)hd;
    if (h->enabled != enabled) heads_changed = 1;
    h->enabled = enabled;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *hd,
                              struct zwlr_output_mode_v1 *m)
{
    struct head *h = data;
    (void)hd;
    for (struct mode *mode = h->modes; mode; mode = mode->next) {
        if (mode->mode == m) {
            if (h->current != mode) heads_changed = 1;
            h->current = mode;
        }
    }
}

static void head_position(void *data, struct zwlr_output_head_v1 *hd, int32_t x, int32_t y)
{
    struct head *h = data;
    (void)hd;
    if (h->x != x || h->y != y) heads_changed = 1;
    h->x = x;
    h->y = y;
}

static void head_transform(void *data, struct zwlr_output_head_v1 *hd, int32_t transform)
{
    (void)data;
    (void)hd;
    (void)transform;
}

static void head_scale(void *data, struct zwlr_output_head_v1 *hd, wl_fixed_t scale)
{
    struct head *h = data;
    (void)hd;
    double value = wl_fixed_to_double(scale);
    if (h->scale != value) heads_changed = 1;
    h->scale = value;
}

static void head_finished(void *data, struct zwlr_output_head_v1 *hd)
{
    struct head *h = data;
    for (struct head **p = &heads; *p; p = &(*p)->next) {
        if (*p == h) {
            *p = h->next;
            break;
        }
    }
    for (struct mode *mode = h->modes; mode; mode = mode->next) {
        mode->head = NULL; /* freed on its own finished event */
    }
    if (output_version >= 3) {
        zwlr_output_head_v1_release(hd);
    } else {
        zwlr_output_head_v1_destroy(hd);
    }
    free(h);
    heads_changed = 1;
}

static void head_adaptive_sync(void *data, struct zwlr_output_head_v1 *hd, uint32_t state)
{
    (void)data;
    (void)hd;
    (void)state;
}

static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name,
    .description = head_string,
    .physical_size = head_physical_size,
    .mode = head_mode,
    .enabled = head_enabled,
    .current_mode = head_current_mode,
    .position = head_position,
    .transform = head_transform,
    .scale = head_scale,
    .finished = head_finished,
    .make = head_string,
    .model = head_string,
    .serial_number = head_string,
    .adaptive_sync = head_adaptive_sync,
};

static void new_head(void *data, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *hd)
{
    (void)data;
    (void)m;
    struct head *h = calloc(1, sizeof *h);
    if (!h) {
        return;
    }
    h->head = hd;
    h->scale = 1.0;
    h->next = heads;
    heads = h;
    zwlr_output_head_v1_add_listener(hd, &head_listener, h);
    heads_changed = 1;
}

static struct mode *pick_mode(struct head *h, const struct monitor_conf *pl)
{
    struct mode *best = NULL;
    for (struct mode *m = h->modes; m; m = m->next) {
        if (pl->width > 0 && (m->width != pl->width || m->height != pl->height)) continue;
        if (pl->refresh_mhz > 0 && m->refresh != pl->refresh_mhz) continue;
        if (m == h->current) return m;
        if (!best || (m->preferred && !best->preferred)) best = m;
    }
    if (!best && pl->width > 0)
        say("labwc-control: %s: saved mode %dx%d@%d unavailable; current mode kept", h->name,
            pl->width, pl->height, pl->refresh_mhz);
    return best ? best : h->current;
}

struct monitor_transaction {
    uint32_t serial;
    int applying, n;
    struct head *heads[16];
    struct mode *modes[16];
    struct monitor_rect rects[16];
    double scales[16];
};
static struct monitor_transaction *pending_monitors;
static void send_monitor_transaction(struct monitor_transaction *t);

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *c)
{
    struct monitor_transaction *t = data;
    zwlr_output_configuration_v1_destroy(c);
    if (!t->applying && t->serial == output_serial && !heads_changed && seat_active) {
        t->applying = 1;
        send_monitor_transaction(t);
        return;
    }
    if (t->applying) say("labwc-control: screens configured");
    else { reload_wanted = 1; apply_at = uptime() + 0.1; }
    pending_monitors = NULL;
    if (reload_wanted) apply_at = uptime() + 0.1;
    free(t);
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *c)
{
    struct monitor_transaction *t = data;
    say("labwc-control: the compositor refused the screen configuration; active layout kept");
    zwlr_output_configuration_v1_destroy(c);
    pending_monitors = NULL;
    if (reload_wanted) apply_at = uptime() + 0.1;
    free(t);
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *c)
{
    struct monitor_transaction *t = data;
    reload_wanted = 1;
    apply_at = uptime() + 0.1;
    zwlr_output_configuration_v1_destroy(c);
    pending_monitors = NULL;
    free(t);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_succeeded,
    .failed = config_failed,
    .cancelled = config_cancelled,
};

static void send_monitor_transaction(struct monitor_transaction *t)
{
    struct zwlr_output_configuration_v1 *c = zwlr_output_manager_v1_create_configuration(outputs, t->serial);
    zwlr_output_configuration_v1_add_listener(c, &config_listener, t);
    for (struct head *hd = heads; hd; hd = hd->next) {
        int i = 0;
        while (i < t->n && t->heads[i] != hd) i++;
        if (i == t->n) { zwlr_output_configuration_v1_disable_head(c, hd->head); continue; }
        struct zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(c, hd->head);
        if (t->modes[i]) zwlr_output_configuration_head_v1_set_mode(ch, t->modes[i]->mode);
        zwlr_output_configuration_head_v1_set_position(ch, t->rects[i].x, t->rects[i].y);
        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(t->scales[i]));
    }
    if (t->applying) zwlr_output_configuration_v1_apply(c);
    else zwlr_output_configuration_v1_test(c);
    wl_display_flush(display);
}

/* Test the connected topology before applying it. A refused transaction
 * leaves the compositor layout active; the saved desire is not rewritten. */
static int apply_monitors(void)
{
    struct head *list[16];
    struct monitor_conf pl[16];
    struct mode *mode[16];
    struct monitor_rect rect[16];
    int n = 0, configured = 0;
    double scale[16];

    if (!outputs) {
        return -1;
    }
    /* A powered-off head must not be disabled in the layout. Its power
     * object can bring it back only while the compositor still owns it. */
    if (pending_monitors || power_phase != POWER_AWAKE || !seat_active) { reload_wanted = 1; return 0; }
    struct monitor_identity connected[MONITOR_PROFILE_MAX];
    int nconnected = monitor_profiles_connected(connected, MONITOR_PROFILE_MAX);
    if (!nconnected) {
        /* Headless tests and a host without DRM sysfs still have the
         * complete output-manager set, including a not-yet-enabled head. */
        for (struct head *hd = heads; hd && nconnected < MONITOR_PROFILE_MAX; hd = hd->next)
            if (hd->name[0]) monitor_identity_sysfs(hd->name, &connected[nconnected++]);
    }
    monitor_profiles_select(connected, nconnected);
    for (struct head *hd = heads; hd && n < 16; hd = hd->next) {
        if (!hd->enabled || !hd->name[0]) {
            continue;
        }
        list[n] = hd;
        configured += monitor_conf(hd->name, &pl[n]);
        mode[n] = pick_mode(hd, &pl[n]);
        scale[n] = pl[n].scale > 0 ? pl[n].scale : hd->scale;
        int place = MONITOR_RIGHT;
        if (strcmp(pl[n].place, "origin") == 0) place = MONITOR_ORIGIN;
        else if (strcmp(pl[n].place, "left") == 0) place = MONITOR_LEFT;
        else if (strcmp(pl[n].place, "above") == 0) place = MONITOR_ABOVE;
        else if (strcmp(pl[n].place, "below") == 0) place = MONITOR_BELOW;
        rect[n] = (struct monitor_rect){ .name = hd->name, .place = place,
            .width = mode[n] ? (int)(mode[n]->width / scale[n]) : 0,
            .height = mode[n] ? (int)(mode[n]->height / scale[n]) : 0,
            .has_position = pl[n].has_position, .x = pl[n].x, .y = pl[n].y };
        n++;
    }
    if (configured == 0) {
        return 0; /* no remembered screens: labwc's own arrangement */
    }
    monitor_layout(rect, (size_t)n);
    int changed = 0;
    for (int i = 0; i < n; i++) {
        changed |= mode[i] != list[i]->current || rect[i].x != list[i]->x || rect[i].y != list[i]->y ||
                   scale[i] - list[i]->scale > 0.001 || list[i]->scale - scale[i] > 0.001;
    }
    if (!changed) {
        return 0;
    }
    struct monitor_transaction *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->serial = output_serial; t->n = n;
    for (int i = 0; i < n; i++) {
        t->heads[i] = list[i]; t->modes[i] = mode[i]; t->rects[i] = rect[i]; t->scales[i] = scale[i];
        say("labwc-control: %s %dx%d scale %.2f at %d,%d", list[i]->name,
            mode[i] ? mode[i]->width : 0, mode[i] ? mode[i]->height : 0, scale[i], rect[i].x, rect[i].y);
    }
    pending_monitors = t;
    send_monitor_transaction(t);
    return 0;
}

static void outputs_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t serial)
{
    (void)data;
    (void)m;
    output_serial = serial;
    if (heads_changed || reload_wanted) {
        heads_changed = reload_wanted = 0;
        apply_monitors();
    }
}

static void outputs_finished(void *data, struct zwlr_output_manager_v1 *m)
{
    (void)data;
    zwlr_output_manager_v1_destroy(m);
    outputs = NULL;
}

static const struct zwlr_output_manager_v1_listener outputs_listener = {
    .head = new_head,
    .done = outputs_done,
    .finished = outputs_finished,
};

/* Output power is separate from layout. Acquire the controls only for an
 * idle cycle; the mode event on acquisition is the initial state, not an
 * acknowledgement. Keep the same controls across OFF and ON, even if a
 * wl_output global goes away, until the power object itself fails. */
struct power_output {
    uint32_t name;
    struct wl_output *output;
    struct zwlr_output_power_v1 *control;
    int known, on, requested_off, owned_off;
    struct power_output *next;
};

static struct zwlr_output_power_manager_v1 *power_manager;
static uint32_t power_manager_name;
static struct power_output *power_outputs;
static struct wl_callback *power_barrier;
static int power_cancelled, power_reported, power_lost, wake_tries, loss_reported;
static int power_inventory_failed;
static double wake_at;

static void power_wake(void);
static void power_sync(void);

static void power_mode(void *data, struct zwlr_output_power_v1 *control, uint32_t mode)
{
    (void)control;
    struct power_output *o = data;
    if (mode != ZWLR_OUTPUT_POWER_V1_MODE_OFF && mode != ZWLR_OUTPUT_POWER_V1_MODE_ON) return;
    o->known = 1;
    o->on = mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
    if (!o->on && o->requested_off) o->owned_off = 1;
    if (o->on) {
        o->owned_off = 0;
        /* The compositor re-enabled a dark head; restore the rest too. */
        if (power_phase == POWER_DARK || (power_phase == POWER_WAKING && !power_barrier)) power_wake();
    }
}

static void power_failed(void *data, struct zwlr_output_power_v1 *control)
{
    struct power_output *o = data;
    if (o->owned_off) power_lost = 1;
    o->known = o->owned_off = 0;
    o->control = NULL;
    zwlr_output_power_v1_destroy(control);
    if (power_phase == POWER_PROBING || power_phase == POWER_BLANKING) power_cancelled = 1;
    if (power_phase == POWER_DARK || (power_phase == POWER_WAKING && !power_barrier)) power_wake();
}

static const struct zwlr_output_power_v1_listener power_listener = { power_mode, power_failed };

static void power_finish(void)
{
    if (power_lost && !loss_reported) idle_report("idle-power-lost");
    else if (!power_lost && power_reported) idle_report("idle-unblanked");
    power_reported = power_lost = loss_reported = 0;
    power_phase = POWER_AWAKE;
    wake_at = 0;
    for (struct power_output **p = &power_outputs; *p;) {
        struct power_output *o = *p;
        if (o->control) zwlr_output_power_v1_destroy(o->control);
        o->control = NULL;
        o->known = o->on = o->requested_off = o->owned_off = 0;
        if (o->output) p = &o->next;
        else { *p = o->next; free(o); }
    }
    if (reload_wanted && seat_active) {
        reload_wanted = 0;
        apply_at = uptime() + 0.1;
    }
}

/* OFF and ON are synchronous commits in labwc, but a KMS commit failure
 * need not produce a failed event. A sync after each batch lets us detect
 * missing mode acknowledgements without a blocking roundtrip. */
static void power_check(void *data, struct wl_callback *cb, uint32_t serial)
{
    (void)data;
    (void)serial;
    wl_callback_destroy(cb);
    power_barrier = NULL;
    if (power_phase == POWER_PROBING) {
        for (struct power_output *o = power_outputs; o; o = o->next) {
            if (!o->output || !o->known) power_cancelled = 1;
        }
        if (power_cancelled || !seat_active) { power_finish(); return; }
        int sent = 0;
        for (struct power_output *o = power_outputs; o; o = o->next) {
            if (!o->on) continue; /* never wake an already-off output */
            o->requested_off = 1;
            zwlr_output_power_v1_set_mode(o->control, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
            sent = 1;
        }
        if (sent) { power_phase = POWER_BLANKING; power_sync(); }
        else power_finish();
    } else if (power_phase == POWER_BLANKING) {
        int owned = 0;
        for (struct power_output *o = power_outputs; o; o = o->next) {
            if (o->requested_off && !o->owned_off) power_cancelled = 1;
            o->requested_off = 0;
            owned |= o->owned_off;
        }
        if (!owned) { power_finish(); return; }
        if (!power_cancelled && seat_active && idle_report("idle-blanked")) {
            power_phase = POWER_DARK;
            power_reported = 1;
        } else {
            power_wake(); /* partial OFF, activity or seat release raced the commit */
        }
    } else if (power_phase == POWER_WAKING) {
        for (struct power_output *o = power_outputs; o; o = o->next) {
            if (!o->owned_off) continue;
            if (seat_active && wake_tries < 4) {
                wake_at = uptime() + 0.1 * wake_tries;
            } else if (seat_active) {
                say("labwc-control: the compositor did not restore all idle-powered outputs");
                if (!loss_reported) loss_reported = idle_report("idle-power-lost");
                /* Keep controls and defer layout changes. Try again on an
                 * explicit wake or when the seat is taken back. */
            }
            return;
        }
        power_finish();
    }
}

static void power_sync(void)
{
    static const struct wl_callback_listener checked = { power_check };
    if (power_barrier) return;
    power_barrier = wl_display_sync(display);
    if (power_barrier) wl_callback_add_listener(power_barrier, &checked, NULL);
    else say("labwc-control: cannot check output power changes");
}

static void power_restore(void)
{
    wake_at = 0;
    int sent = 0;
    for (struct power_output *o = power_outputs; o; o = o->next) {
        if (!o->owned_off) continue;
        if (!seat_active) return;
        if (!o->control) { power_lost = 1; continue; }
        zwlr_output_power_v1_set_mode(o->control, ZWLR_OUTPUT_POWER_V1_MODE_ON);
        sent = 1;
    }
    if (sent) { wake_tries++; power_sync(); }
    else power_finish();
}

static void power_wake(void)
{
    if (power_phase == POWER_AWAKE) return;
    if (power_barrier) {
        if (power_phase == POWER_PROBING || power_phase == POWER_BLANKING) power_cancelled = 1;
        return;
    }
    power_phase = POWER_WAKING;
    wake_tries = 0;
    power_restore();
}

static void power_blank(void)
{
    if (!seat_active || !power_manager || !power_outputs || power_inventory_failed ||
        pending_monitors || power_phase != POWER_AWAKE) return;
    power_cancelled = 0;
    power_phase = POWER_PROBING;
    for (struct power_output *o = power_outputs; o; o = o->next) {
        o->control = zwlr_output_power_manager_v1_get_output_power(power_manager, o->output);
        if (o->control) zwlr_output_power_v1_add_listener(o->control, &power_listener, o);
    }
    power_sync();
}

/* Idleness: the notifications that respect idle inhibitors ask for the lock
 * and display-off. The input-only notification only logs the lack of input. */

static struct ext_idle_notification_v1 *input_idle;

static void idled(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data;
    if (!seat_active) return;
    if (n == input_idle) {
        say("labwc-control: no input for %d minutes", idle_minutes);
    } else if (n == blank_idle) {
        power_blank();
    } else if (n == idle) {
        say("labwc-control: no input for %d minutes, and nothing holds idleness off: asking for the lock",
            idle_minutes);
        idle_report("idle-lock");
    }
}

static void resumed(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data;
    if (n == blank_idle) power_wake();
}

static const struct ext_idle_notification_v1_listener idle_listener = { idled, resumed };

struct idle_rc {
    int minutes;
    int auto_lock;
    int lock_high;
    int auto_blank;
    int blank_after;
};

static void idle_rc_setting(const char *name, const char *value, void *arg)
{
    struct idle_rc *rc = arg;
    if (strcmp(name, "lock_after") == 0) {
        rc->minutes = atoi(value) > 0 ? atoi(value) : 0;
    } else if (strcmp(name, "auto_lock") == 0) {
        rc->auto_lock = strcmp(value, "0") != 0;
    } else if (strcmp(name, "lock_mode") == 0) {
        rc->lock_high = strcmp(value, "high") == 0;
    } else if (strcmp(name, "auto_blank") == 0) {
        rc->auto_blank = strcmp(value, "1") == 0;
    } else if (strcmp(name, "blank_after") == 0) {
        size_t len = strlen(value);
        int minutes = len > 0 && len < 6 && strspn(value, "0123456789") == len ? atoi(value) : 0;
        rc->blank_after = minutes >= 1 && minutes <= 60 ? minutes : 5;
    }
}

static void reset_idle(struct ext_idle_notification_v1 **n, int minutes, int input_only)
{
    if (*n) ext_idle_notification_v1_destroy(*n);
    *n = NULL;
    if (minutes <= 0) return;
    uint32_t timeout = (uint32_t)minutes * 60000;
    *n = input_only ? ext_idle_notifier_v1_get_input_idle_notification(idle_notifier, timeout, seat) :
        ext_idle_notifier_v1_get_idle_notification(idle_notifier, timeout, seat);
    if (*n) ext_idle_notification_v1_add_listener(*n, &idle_listener, NULL);
}

/* Independent timers: a display-off must not postpone a due idle-lock. */
static void watch_idle(void)
{
    struct idle_rc rc = { .minutes = 10, .auto_lock = 1, .blank_after = 5 };
    rc_read(idle_rc_setting, &rc);
    int available = seat_active && seat_requests >= 0 && idle_notifier && seat;
    int minutes = available && (rc.auto_lock || rc.lock_high) ? rc.minutes : 0;
    int blank = available && rc.auto_blank && power_manager && !power_inventory_failed ? rc.blank_after : 0;
    if (blank != blank_minutes) {
        power_wake();
        reset_idle(&blank_idle, blank, 0);
        blank_minutes = blank;
    }
    if (minutes != idle_minutes) {
        reset_idle(&idle, minutes, 0);
        int log_input = minutes > 0 && ext_idle_notifier_v1_get_version(idle_notifier) >= 2;
        reset_idle(&input_idle, log_input ? minutes : 0, 1);
        idle_minutes = minutes;
    }
}

static void seat_request(void)
{
    char msg[32];
    ssize_t n = recv(seat_requests, msg, sizeof msg, MSG_DONTWAIT | MSG_TRUNC);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        close(seat_requests);
        seat_requests = -1;
        /* If still active, restore what we switched off before parking the timers. */
        watch_idle();
    } else if (n == 12 && memcmp(msg, "seat-release", 12) == 0) {
        seat_active = 0;
        power_wake();
        watch_idle();
    } else if (n == 9 && memcmp(msg, "seat-take", 9) == 0) {
        seat_active = 1;
        idle_minutes = blank_minutes = -1;
        watch_idle(); /* also restores any retained OFF controls */
        if (reload_wanted && power_phase == POWER_AWAKE) apply_at = uptime() + 0.1;
    } else if (n == 11 && memcmp(msg, "wake-screen", 11) == 0) {
        power_wake();
    } else if (n == 12 && memcmp(msg, "bar-activity", 12) == 0) {
        power_wake();
        /* Idle timers restart at most every 2 s: rapid activity never makes
         * labwc destroy and create its notifications for each message. */
        static double last_reset = -10.0;
        double now = uptime();
        if (seat_active && idle_notifier && seat && now - last_reset >= 2.0) {
            last_reset = now;
            if (blank_minutes > 0) reset_idle(&blank_idle, blank_minutes, 0);
            if (idle_minutes > 0) {
                int log_input = ext_idle_notifier_v1_get_version(idle_notifier) >= 2;
                reset_idle(&idle, idle_minutes, 0);
                reset_idle(&input_idle, log_input ? idle_minutes : 0, 1);
            }
        }
    }
}

/* Setup */

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct power_output *o = calloc(1, sizeof *o);
        if (o) {
            o->output = wl_registry_bind(registry, name, &wl_output_interface, version < 3 ? version : 3);
            if (o->output) {
                o->name = name;
                o->next = power_outputs;
                power_outputs = o;
            } else {
                free(o);
                power_inventory_failed = 1;
            }
        } else {
            power_inventory_failed = 1;
        }
        /* A changed inventory is not the one we checked before blanking. */
        power_wake();
    } else if (strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0 && !power_manager) {
        power_manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
        power_manager_name = name;
        watch_idle();
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        idle_notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, version < 2 ? version : 2);
    } else if (strcmp(interface, wp_security_context_manager_v1_interface.name) == 0) {
        security = wl_registry_bind(registry, name, &wp_security_context_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        toplevels = wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                     version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevels, &windows_listener, NULL);
    } else if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        output_version = version < 4 ? version : 4;
        outputs = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface,
                                   output_version);
        zwlr_output_manager_v1_add_listener(outputs, &outputs_listener, NULL);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    if (power_manager && name == power_manager_name) {
        zwlr_output_power_manager_v1_destroy(power_manager);
        power_manager = NULL;
        watch_idle();
        return;
    }
    for (struct power_output **p = &power_outputs; *p; p = &(*p)->next) {
        struct power_output *o = *p;
        if (!o->output || o->name != name) continue;
        if (wl_output_get_version(o->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) wl_output_release(o->output);
        else wl_output_destroy(o->output);
        o->output = NULL;
        if (power_phase == POWER_AWAKE) { *p = o->next; free(o); }
        /* During a power cycle global removal alone does not invalidate
         * the retained control. Its failed event does. */
        break;
    }
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static int unix_listener(const char *path, int type, mode_t mode)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    unlink(path);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        chmod(path, mode) < 0 || listen(fd, 16) < 0) {
        say("labwc-control: cannot listen on %s: %s", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

/* The containers' display; returns the end that keeps it open, or -1. */
static int open_container_display(void)
{
    int fd = unix_listener(container_path, SOCK_STREAM, 0666), keep[2];
    if (fd < 0 || pipe2(keep, O_CLOEXEC) < 0) {
        return -1;
    }
    struct wp_security_context_v1 *context =
        wp_security_context_manager_v1_create_listener(security, fd, keep[0]);
    wp_security_context_v1_set_sandbox_engine(context, "locachy");
    wp_security_context_v1_set_app_id(context, "container");
    wp_security_context_v1_commit(context);
    wp_security_context_v1_destroy(context);
    int ok = wl_display_roundtrip(display) >= 0;
    close(fd);
    close(keep[0]);
    if (!ok) {
        close(keep[1]);
        return -1;
    }
    return keep[1];
}

/* window PROGRAM [TARGET] (locachy-seat --window): a host program as a
 * managed window. Its fields, bounded, as process 1 takes them; the seat and
 * process 1 check program and target. */
static void window_request(const char *args, char *reply, size_t size)
{
    char msg[16 + 64 + 1024];
    const char *space = strchr(args, ' ');
    size_t plen = space ? (size_t)(space - args) : strlen(args);
    const char *target = space ? space + 1 : "";
    if (seat_requests < 0) {
        snprintf(reply, size, "error no seat to pass window requests to");
        return;
    }
    if (plen == 0 || plen >= 64 || strlen(target) >= 1024) {
        snprintf(reply, size, "error malformed window request");
        return;
    }
    int n = snprintf(msg, sizeof msg, "window%c%.*s", '\0', (int)plen, args);
    if (target[0]) {
        n += snprintf(msg + n + 1, sizeof msg - (size_t)n - 1, "%s", target) + 1;
    }
    if (send(seat_requests, msg, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) != n) {
        snprintf(reply, size, "error the seat does not take window requests now");
        return;
    }
    snprintf(reply, size, "ok window requested");
}

static void open_request(const char *name, int own_tor, char *reply, size_t size)
{
    char msg[32 + 64];
    if (seat_requests < 0) {
        snprintf(reply, size, "error no seat to pass window requests to");
        return;
    }
    if (!valid_name(name) || strncmp(name, "window-", 7) == 0) {
        snprintf(reply, size, "error malformed container request");
        return;
    }
    int n = snprintf(msg, sizeof msg, "app%c%s%c%s", '\0', own_tor ? "open-own-tor" : "open", '\0', name);
    if (send(seat_requests, msg, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) != n) {
        snprintf(reply, size, "error the seat does not take window requests now");
        return;
    }
    snprintf(reply, size, "ok open requested");
}

static void serve(int listener)
{
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        return;
    }
    char request[16 + 64 + 1024], reply[160];
    struct pollfd wait = { .fd = client, .events = POLLIN };
    ssize_t n = poll(&wait, 1, 200) > 0 ? recv(client, request, sizeof request - 1, 0) : -1;
    if (n <= 0) {
        close(client);
        return;
    }
    request[n] = '\0';
    if (strncmp(request, "show ", 5) == 0 || strncmp(request, "raise ", 6) == 0) {
        int bar = request[0] == 's';
        wl_display_roundtrip(display); /* the windows as they are now */
        show(request + (bar ? 5 : 6), reply, sizeof reply, bar);
    } else if (strncmp(request, "close ", 6) == 0) {
        wl_display_roundtrip(display);
        close_windows(request + 6, reply, sizeof reply);
    } else if (strncmp(request, "window ", 7) == 0) {
        window_request(request + 7, reply, sizeof reply);
    } else if (strncmp(request, "open ", 5) == 0) {
        open_request(request + 5, 0, reply, sizeof reply);
    } else if (strncmp(request, "open-own-tor ", 13) == 0) {
        open_request(request + 13, 1, reply, sizeof reply);
    } else if (strcmp(request, "focused") == 0) {
        wl_display_roundtrip(display);
        focused(reply, sizeof reply);
    } else if (strcmp(request, "reload-monitors") == 0) {
        wl_display_roundtrip(display);
        snprintf(reply, sizeof reply, apply_monitors() == 0 ? "ok monitors" :
                 "error the compositor offers no output management");
    } else if (strcmp(request, "home") == 0 || strcmp(request, "lock") == 0 || strcmp(request, "next") == 0 ||
               strcmp(request, "prev") == 0) {
        snprintf(reply, sizeof reply, "ok %.15s", request); /* home, lock, next or prev */
        send(client, reply, strlen(reply), MSG_NOSIGNAL);
        close(client);
        /* To locachy-seat, which asks process 1: home, lock, the next or
         * previous task (a touchpad swipe, as in the direct programs). */
        kill(getppid(), request[0] == 'h' ? SIGUSR1 : request[0] == 'l' ? SIGUSR2 :
                        request[0] == 'n' ? SIGRTMIN : SIGRTMIN + 1);
        return;
    } else {
        snprintf(reply, sizeof reply, "error unknown request");
    }
    send(client, reply, strlen(reply), MSG_NOSIGNAL);
    close(client);
}

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

static int send_request(const char *msg, char *reply, size_t rsize)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') return 1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int n = snprintf(address.sun_path, sizeof address.sun_path, "%s/locachy-control.sock", runtime);
    if (n < 0 || n >= (int)sizeof address.sun_path) return 1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return 1;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        send(fd, msg, strlen(msg), MSG_NOSIGNAL) != (ssize_t)strlen(msg)) {
        close(fd);
        return 1;
    }
    struct pollfd wait = { .fd = fd, .events = POLLIN };
    ssize_t got = poll(&wait, 1, 1000) > 0 ? recv(fd, reply, rsize - 1, 0) : -1;
    close(fd);
    if (got <= 0) return 1;
    reply[got] = '\0';
    return strncmp(reply, "ok ", 3) == 0 ? 0 : 1;
}

static int send_action(const char *action)
{
    char reply[64] = "";
    if (strcmp(action, "home") != 0 && strcmp(action, "lock") != 0 && strcmp(action, "next") != 0 &&
        strcmp(action, "prev") != 0) {
        return 2;
    }
    return send_request(action, reply, sizeof reply);
}

static int send_close(const char *name)
{
    char msg[96], reply[64] = "";
    if (!valid_name(name)) return 2;
    snprintf(msg, sizeof msg, "close %.63s", name);
    if (send_request(msg, reply, sizeof reply) != 0) return 1;
    int count = 0;
    return sscanf(reply, "ok closed %d", &count) == 1 && count > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--action") == 0) return send_action(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--close") == 0) return send_close(argv[2]);
    if (argc != 1) return 2;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        say("labwc-control: XDG_RUNTIME_DIR is not set");
        return 1;
    }
    snprintf(container_path, sizeof container_path, "%s/locachy-0", runtime);
    snprintf(control_path, sizeof control_path, "%s/locachy-control.sock", runtime);
    snprintf(bar_path, sizeof bar_path, "%s/" BAR_SOCKET, runtime);
    const char *requests = getenv("LOCACHY_SEAT_REQUESTS");
    if (requests && *requests) {
        seat_requests = atoi(requests);
        if (seat_requests < 3 || fcntl(seat_requests, F_SETFD, FD_CLOEXEC) < 0) {
            seat_requests = -1;
        }
    }
    unsetenv("LOCACHY_SEAT_REQUESTS");
    for (int tries = 0; !display && tries < 100; tries++) {
        display = wl_display_connect(NULL);
        if (!display) {
            usleep(50000);
        }
    }
    if (!display) {
        say("labwc-control: cannot connect to the compositor");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); /* the heads, modes and windows there are */
    if (!security) {
        say("labwc-control: the compositor has no security contexts; no display for containers");
        return 1;
    }
    int keep = open_container_display();
    int listener = unix_listener(control_path, SOCK_SEQPACKET, 0600);
    if (keep < 0 || listener < 0) {
        unlink(container_path);
        unlink(control_path);
        return 1;
    }
    int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch >= 0 && inotify_add_watch(watch, "/etc/locachy", IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(watch);
        watch = -1;
    }
    watch_idle();
    say("labwc-control: containers' display %s (window control %s, output management %s, idle notify %s, output power %s)",
        container_path, toplevels ? "yes" : "no", outputs ? "yes" : "no", idle_notifier ? "yes" : "no",
        power_manager ? "yes" : "no");
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    while (!stop) {
        struct pollfd p[] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = listener, .events = POLLIN },
            { .fd = watch, .events = POLLIN },
            { .fd = seat_requests, .events = POLLIN },
        };
        double next = apply_at > 0 && (wake_at <= 0 || apply_at < wake_at) ? apply_at : wake_at;
        double left = next - uptime();
        int timeout = next <= 0 ? -1 : left > 0 ? (int)(left * 1000) + 1 : 0;
        if (wl_display_flush(display) < 0 && errno == EAGAIN) p[0].events |= POLLOUT;
        if (poll(p, sizeof p / sizeof *p, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (p[3].revents & (POLLIN | POLLHUP | POLLERR)) {
            /* Take/release before activity already queued by the compositor. */
            seat_request();
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (wl_display_dispatch(display) < 0) {
                break; /* the compositor has gone */
            }
        }
        if (p[1].revents & POLLIN) {
            serve(listener);
        }
        if (p[2].revents & POLLIN) {
            char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t n = read(watch, events, sizeof events);
            for (char *e = events; n > 0 && e < events + n;) {
                struct inotify_event *ev = (struct inotify_event *)e;
                if (ev->len && (!strcmp(ev->name, strrchr(MONITORS_CONF, '/') + 1) ||
                                !strcmp(ev->name, strrchr(MONITOR_PROFILES_CONF, '/') + 1))) {
                    apply_at = uptime() + 0.3;
                } else if (ev->len && strcmp(ev->name, strrchr(RC_CONF, '/') + 1) == 0) {
                    watch_idle();
                }
                e += sizeof *ev + ev->len;
            }
        }
        if (wake_at > 0 && uptime() >= wake_at && power_phase == POWER_WAKING) power_restore();
        if (apply_at > 0 && uptime() >= apply_at) {
            apply_at = 0;
            wl_display_roundtrip(display); /* the screens as they are now */
            apply_monitors();
        }
    }
    unlink(container_path);
    unlink(control_path);
    close(keep);
    return 0;
}
