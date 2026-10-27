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
 *   reload-monitors  apply MONITORS_CONF: "ok monitors" or "error ..."
 *   focused          the container of the focused window: "ok NAME", or "ok"
 *                    when no container's window has the focus
 * A window belongs to container NAME when its app_id names a .desktop file
 * that NAME installed (or that file's StartupWMClass), the way desktop shells
 * match windows to applications.
 *
 * Screens: MONITORS_CONF, written by settings, is applied at start, on
 * request, whenever it changes (settings writes it while the shared display
 * is frozen: it is applied once that runs again) and whenever a screen is
 * connected or disconnected; a configuration the compositor refuses (as
 * while it is getting the screens back) is tried again, up to 5 times. Its lines are
 * CONNECTOR=PLACE,MODE,SCALE: PLACE is origin (the screen the others are
 * placed around) or right, left, above, below; MODE is WIDTHxHEIGHT or
 * preferred; SCALE may be fractional. A screen it does not name keeps its
 * mode and scale and goes to the right.
 *
 * Idleness: after rc.conf's lock_after minutes (default 10; 0: never) without
 * input on the shared display, as labwc tells (ext-idle-notify-v1, which
 * respects what holds it off: a video playing asks for that), its parent,
 * locachy-seat, is sent SIGUSR2: the seat asks process 1 to lock the screen,
 * which it does while the shared display has the screen. Asked again
 * whenever rc.conf changes, and when the shared display gets the screen back
 * (its screens come back): switching to it was input that labwc did not see,
 * and an idleness labwc reported while it was away would otherwise not be
 * reported again until labwc sees input.
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
#include "base.h"
#include "monitor-layout.h"

#define MAX_IDS 16
#define BAR_SOCKET "locachy-bar.sock"

static struct wl_display *display;
static struct wl_seat *seat;
static struct wp_security_context_manager_v1 *security;
static struct zwlr_foreign_toplevel_manager_v1 *toplevels;
static struct zwlr_output_manager_v1 *outputs;
static uint32_t output_version;
static struct ext_idle_notifier_v1 *idle_notifier;
static struct ext_idle_notification_v1 *idle;
static int idle_minutes = -1; /* what idle was asked for with; -1: ask again */
static char container_path[108], control_path[108], bar_path[108];

/* Windows */

struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
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
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(w);
}

static void window_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                          struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data;
    (void)h;
    (void)parent;
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
    int nids = open_window_ids(name, ids, MAX_IDS);
    int shown = 0;
    for (struct window *w = windows; w; w = w->next) {
        for (int i = 0; i < nids; i++) {
            if (strcasecmp(w->app_id, ids[i]) == 0) {
                if (w->minimized) {
                    zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
                }
                zwlr_foreign_toplevel_handle_v1_activate(w->handle, seat);
                shown++;
                break;
            }
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

static void focused(char *reply, size_t size)
{
    struct window *w = windows;
    while (w && !w->activated) {
        w = w->next;
    }
    snprintf(reply, size, "ok");
    char names[64][OPEN_NAME];
    int n = w ? open_containers(names, 64) : 0;
    for (int k = 0; k < n; k++) {
        char ids[MAX_IDS][OPEN_WINDOW_ID];
        int nids = open_window_ids(names[k], ids, MAX_IDS);
        for (int i = 0; i < nids; i++) {
            if (strcasecmp(w->app_id, ids[i]) == 0) {
                snprintf(reply, size, "ok %.63s", names[k]);
                return;
            }
        }
    }
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
static int heads_changed, reload_wanted, refused;
static double apply_at; /* apply MONITORS_CONF then (uptime), if not 0 */

static void mode_size(void *data, struct zwlr_output_mode_v1 *m, int32_t width, int32_t height)
{
    struct mode *mode = data;
    (void)m;
    mode->width = width;
    mode->height = height;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *m, int32_t refresh)
{
    struct mode *mode = data;
    (void)m;
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
    h->enabled = enabled;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *hd,
                              struct zwlr_output_mode_v1 *m)
{
    struct head *h = data;
    (void)hd;
    for (struct mode *mode = h->modes; mode; mode = mode->next) {
        if (mode->mode == m) {
            h->current = mode;
        }
    }
}

static void head_position(void *data, struct zwlr_output_head_v1 *hd, int32_t x, int32_t y)
{
    struct head *h = data;
    (void)hd;
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
    h->scale = wl_fixed_to_double(scale);
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
        if (pl->width > 0) {
            if (m->width != pl->width || m->height != pl->height) {
                continue;
            }
            if (!best || (m->preferred && !best->preferred) ||
                (m->preferred == best->preferred && m->refresh > best->refresh)) {
                best = m;
            }
        } else if (m->preferred) {
            best = m;
            break;
        }
    }
    return best ? best : h->current;
}

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *c)
{
    (void)data;
    refused = 0;
    say("labwc-control: screens configured");
    zwlr_output_configuration_v1_destroy(c);
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *c)
{
    (void)data;
    say("labwc-control: the compositor refused the screen configuration%s",
        refused < 5 ? "; trying again in a second" : "");
    if (refused++ < 5) {
        apply_at = uptime() + 1.0;
    }
    zwlr_output_configuration_v1_destroy(c);
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *c)
{
    (void)data;
    reload_wanted = 1; /* the screens changed meanwhile: again, after their done */
    zwlr_output_configuration_v1_destroy(c);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_succeeded,
    .failed = config_failed,
    .cancelled = config_cancelled,
};

/* Apply MONITORS_CONF to the enabled screens; 0 if sent or nothing to do. */
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
            .height = mode[n] ? (int)(mode[n]->height / scale[n]) : 0 };
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
    struct zwlr_output_configuration_v1 *config =
        zwlr_output_manager_v1_create_configuration(outputs, output_serial);
    zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
    for (struct head *hd = heads; hd; hd = hd->next) {
        int i = 0;
        while (i < n && list[i] != hd) {
            i++;
        }
        if (i == n) {
            zwlr_output_configuration_v1_disable_head(config, hd->head);
            continue;
        }
        struct zwlr_output_configuration_head_v1 *ch =
            zwlr_output_configuration_v1_enable_head(config, hd->head);
        if (mode[i]) {
            zwlr_output_configuration_head_v1_set_mode(ch, mode[i]->mode);
        }
        zwlr_output_configuration_head_v1_set_position(ch, rect[i].x, rect[i].y);
        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(scale[i]));
        say("labwc-control: %s %dx%d scale %.2f at %d,%d", hd->name,
            mode[i] ? mode[i]->width : 0, mode[i] ? mode[i]->height : 0, scale[i], rect[i].x, rect[i].y);
    }
    zwlr_output_configuration_v1_apply(config);
    wl_display_flush(display);
    return 0;
}

static void watch_idle(void);

static void outputs_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t serial)
{
    (void)data;
    (void)m;
    static int had_heads;
    output_serial = serial;
    if (heads != NULL && !had_heads) {
        idle_minutes = -1; /* the screen is back: idleness counted from now */
        watch_idle();
    }
    had_heads = heads != NULL;
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

/* Idleness: the notification that respects idle inhibitors asks for the
 * lock; one of input alone (version 2), only to log that there was no input
 * though a program held idleness off (labwc does not say which). */

static struct ext_idle_notification_v1 *input_idle;

static void idled(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data;
    if (n == input_idle) {
        say("labwc-control: no input for %d minutes", idle_minutes);
        return;
    }
    say("labwc-control: no input for %d minutes, and nothing holds idleness off: asking for the lock",
        idle_minutes);
    kill(getppid(), SIGUSR2);
}

static void resumed(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data;
    (void)n;
}

static const struct ext_idle_notification_v1_listener idle_listener = { idled, resumed };

static void lock_after_setting(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "lock_after") == 0) {
        *(int *)arg = atoi(value) > 0 ? atoi(value) : 0;
    }
}

/* rc.conf's lock_after, asked of labwc again if it changed. */
static void watch_idle(void)
{
    int minutes = 10;
    rc_read(lock_after_setting, &minutes);
    if (minutes == idle_minutes || !idle_notifier || !seat) {
        return;
    }
    if (idle) {
        ext_idle_notification_v1_destroy(idle);
        idle = NULL;
    }
    if (input_idle) {
        ext_idle_notification_v1_destroy(input_idle);
        input_idle = NULL;
    }
    idle_minutes = minutes;
    if (minutes > 0) {
        idle = ext_idle_notifier_v1_get_idle_notification(idle_notifier, (uint32_t)minutes * 60000, seat);
        ext_idle_notification_v1_add_listener(idle, &idle_listener, NULL);
        if (ext_idle_notifier_v1_get_version(idle_notifier) >= 2) {
            input_idle = ext_idle_notifier_v1_get_input_idle_notification(idle_notifier,
                                                                          (uint32_t)minutes * 60000, seat);
            ext_idle_notification_v1_add_listener(input_idle, &idle_listener, NULL);
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
    (void)name;
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

static void serve(int listener)
{
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        return;
    }
    char request[128], reply[160];
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
    } else if (strcmp(request, "focused") == 0) {
        wl_display_roundtrip(display);
        focused(reply, sizeof reply);
    } else if (strcmp(request, "reload-monitors") == 0) {
        wl_display_roundtrip(display);
        snprintf(reply, sizeof reply, apply_monitors() == 0 ? "ok monitors" :
                 "error the compositor offers no output management");
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

int main(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        say("labwc-control: XDG_RUNTIME_DIR is not set");
        return 1;
    }
    snprintf(container_path, sizeof container_path, "%s/locachy-0", runtime);
    snprintf(control_path, sizeof control_path, "%s/locachy-control.sock", runtime);
    snprintf(bar_path, sizeof bar_path, "%s/" BAR_SOCKET, runtime);
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
    say("labwc-control: containers' display %s (window control %s, output management %s, idleness %s)",
        container_path, toplevels ? "yes" : "no", outputs ? "yes" : "no", idle_notifier ? "yes" : "no");
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    while (!stop) {
        struct pollfd p[3] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = listener, .events = POLLIN },
            { .fd = watch, .events = POLLIN }, /* -1: ignored */
        };
        int timeout = -1;
        if (apply_at > 0) {
            double left = apply_at - uptime();
            timeout = left > 0 ? (int)(left * 1000) + 1 : 0;
        }
        wl_display_flush(display);
        if (poll(p, 3, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
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
                if (ev->len && strcmp(ev->name, strrchr(MONITORS_CONF, '/') + 1) == 0) {
                    refused = 0;
                    apply_at = uptime() + 0.3; /* settled: written, and renamed */
                } else if (ev->len && strcmp(ev->name, strrchr(RC_CONF, '/') + 1) == 0) {
                    watch_idle();
                }
                e += sizeof *ev + ev->len;
            }
        }
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
