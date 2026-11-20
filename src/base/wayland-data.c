/* Standard Wayland clipboard, primary selection and drag source for windows
 * of the shared software canvas (LOCACHY_BACKEND=wayland): ordinary
 * wl_data_device and primary-selection transfers, no host clipboard socket
 * and no compositor-private protocol. Writes to requesters are nonblocking
 * with a deadline; a read returns a sealed memfd with the exact size, as the
 * host clipboard service does. Direct and static builds compile stubs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WL_DATA_IMPLEMENTATION
#include "clipboard.h"
#include "include/wayland-backend.h"
#include "include/wayland-data.h"

#ifdef CONFIG_WAYLAND
#include <wayland-client.h>
#include "primary-selection-unstable-v1-client-protocol.h"

#define WRITES_MAX 16
#define WRITE_TIMEOUT_MS 3000
#define DATA_LIMIT ((size_t)64 * 1024 * 1024) /* as the host clipboard service */

enum { M_UTF8 = 1, M_UTF8_STRING = 2, M_TEXT = 4, M_PNG = 8, M_URI = 16, M_TRANSFER = 32, M_FILES = 64 };
#define M_ANY_TEXT (M_UTF8 | M_UTF8_STRING | M_TEXT)
#define M_KEY (M_TRANSFER | M_FILES) /* a document-portal file transfer key */
static const struct { const char *name; unsigned bit; } mime_names[] = {
    { "text/plain;charset=utf-8", M_UTF8 }, { "UTF8_STRING", M_UTF8_STRING },
    { "text/plain", M_TEXT }, { "image/png", M_PNG }, { "text/uri-list", M_URI },
    { "application/vnd.portal.filetransfer", M_TRANSFER }, { "application/vnd.portal.files", M_FILES },
};
#define NMIMES (sizeof mime_names / sizeof mime_names[0])

static unsigned mime_bit(const char *mime)
{
    for (size_t i = 0; mime && i < NMIMES; i++) {
        if (!strcmp(mime, mime_names[i].name)) return mime_names[i].bit;
    }
    return 0;
}

/* Bytes shared by an offered selection and its writes in progress. */
struct blob {
    size_t refs, size;
    char data[];
};

static struct blob *blob_new(const void *data, size_t size)
{
    if (size > DATA_LIMIT) return NULL;
    struct blob *b = malloc(sizeof *b + size + 1);
    if (!b) return NULL;
    b->refs = 1;
    b->size = size;
    if (size) memcpy(b->data, data, size);
    b->data[size] = '\0';
    return b;
}

static void blob_put(struct blob *b)
{
    if (b && --b->refs == 0) free(b);
}

/* What this client offers: a selection (regular, primary) or a drag. */
struct offered {
    void *source; /* wl_data_source, or zwp_primary_selection_source_v1 */
    struct blob *blob, *key; /* key: what M_KEY types get (a drag with one) */
    unsigned kind;
    int primary;
};

/* The receive request hands this exact pipe to the selection's source. */
struct selection_probe {
    struct offered *source;
    struct stat pipe;
    int matched;
};

struct pending {
    int fd;
    struct blob *blob;
    size_t done;
    int64_t until;
};

static struct {
    struct wl_display *display;
    struct wl_seat *seat;
    struct wl_surface *surface;
    struct wl_data_device_manager *manager;
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct wl_data_device *device;
    struct zwp_primary_selection_device_v1 *primary_device;
    /* Received: the regular and primary selections, and a drag over us. */
    struct wl_data_offer *offer, *dnd_offer;
    struct zwp_primary_selection_offer_v1 *primary_offer;
    struct offered slots[CLIPBOARD_SELECTIONS][2], *own[CLIPBOARD_SELECTIONS], drag;
    struct selection_probe *probe;
    unsigned observed_focus[CLIPBOARD_SELECTIONS], observed_revision[CLIPBOARD_SELECTIONS];
    int observed[CLIPBOARD_SELECTIONS], selection_busy;
    int drag_active, drag_dropped, drag_finished;
    /* Told once when a drag with a key ends: finished 1 when a target read
     * it, 0 when it was cancelled (or the data device went). */
    void (*drag_ended)(int finished, void *data);
    void *drag_ended_data;
    struct pending writes[WRITES_MAX];
    size_t nwrites;
} ds;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A requester may close its end first: no SIGPIPE for the program. */
static ssize_t write_quiet(int fd, const void *buf, size_t size)
{
    sigset_t pipe_set, old, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    int was_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
    sigprocmask(SIG_BLOCK, &pipe_set, &old);
    ssize_t n = write(fd, buf, size);
    int err = errno;
    if (n < 0 && err == EPIPE && !was_pending) {
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {}
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = err;
    return n;
}

static void queue_write(int fd, struct blob *b)
{
    int flags = fcntl(fd, F_GETFL);
    if (!b || ds.nwrites == WRITES_MAX || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    b->refs++;
    ds.writes[ds.nwrites++] = (struct pending){ .fd = fd, .blob = b, .until = now_ms() + WRITE_TIMEOUT_MS };
}

static void write_end(size_t i)
{
    close(ds.writes[i].fd);
    blob_put(ds.writes[i].blob);
    ds.writes[i] = ds.writes[--ds.nwrites];
}

/* The end of a drag with a key, told once. */
static void drag_end(int finished)
{
    void (*ended)(int, void *) = ds.drag_ended;
    void *data = ds.drag_ended_data;
    ds.drag_ended = NULL;
    ds.drag_ended_data = NULL;
    if (ended) ended(finished, data);
}

static void offered_clear(struct offered *o)
{
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        if (ds.own[i] == o) ds.own[i] = NULL;
    }
    if (o->source) {
        if (o->primary) zwp_primary_selection_source_v1_destroy(o->source);
        else wl_data_source_destroy(o->source);
    }
    blob_put(o->blob);
    blob_put(o->key);
    memset(o, 0, sizeof *o);
    if (o == &ds.drag) {
        ds.drag_active = ds.drag_dropped = 0;
        if (ds.drag_ended) drag_end(0); /* gone without a finish: cancelled */
    }
}

static int accept_probe(struct offered *o, int fd)
{
    struct selection_probe *probe = ds.probe;
    if (!probe || probe->source != o) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISFIFO(st.st_mode) || st.st_dev != probe->pipe.st_dev ||
        st.st_ino != probe->pipe.st_ino) return 0;
    probe->matched = 1;
    close(fd); /* Ownership was proved; no payload needs to cross the pipe. */
    return 1;
}

static void selection_observed(int which)
{
    ds.observed[which] = wl_backend_keyboard_focused();
    ds.observed_focus[which] = wl_backend_keyboard_focus_generation();
    ds.observed_revision[which]++;
}

static void data_offer_drop(struct wl_data_offer **offer)
{
    if (*offer) {
        free(wl_data_offer_get_user_data(*offer));
        wl_data_offer_destroy(*offer);
        *offer = NULL;
    }
}

static void primary_offer_drop(struct zwp_primary_selection_offer_v1 **offer)
{
    if (*offer) {
        free(zwp_primary_selection_offer_v1_get_user_data(*offer));
        zwp_primary_selection_offer_v1_destroy(*offer);
        *offer = NULL;
    }
}

/* --- What we offer --- */

static void source_target(void *data, struct wl_data_source *src, const char *mime)
{
    (void)data; (void)src; (void)mime;
}

static void source_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd)
{
    struct offered *o = data;
    unsigned bit = mime_bit(mime);
    if (o->source == src && (bit & o->kind)) {
        if (accept_probe(o, fd)) return;
        queue_write(fd, bit & M_KEY ? o->key : o->blob);
        /* Before version 3 nothing else says a drop was read (a drag with a
         * key needs version 3: receivers ask for it on entering). */
        if (o == &ds.drag && !o->key && wl_proxy_get_version((struct wl_proxy *)src) < 3) {
            ds.drag_active = 0;
            ds.drag_finished = 1;
        }
    } else {
        close(fd);
    }
}

static void source_cancelled(void *data, struct wl_data_source *src)
{
    struct offered *o = data;
    if (o->source != src) return;
    if (o == &ds.drag) {
        ds.drag_active = 0; /* refused, or dropped where nothing takes it */
    }
    offered_clear(o);
}

static void source_drop_performed(void *data, struct wl_data_source *src)
{
    struct offered *o = data;
    if (o == &ds.drag && o->source == src) ds.drag_dropped = 1;
}

static void source_finished(void *data, struct wl_data_source *src)
{
    struct offered *o = data;
    if (o != &ds.drag || o->source != src) return;
    ds.drag_active = 0;
    ds.drag_finished = 1;
    drag_end(1);
    offered_clear(o); /* writes in progress keep their own reference */
}

static void source_action(void *data, struct wl_data_source *src, uint32_t action)
{
    (void)data; (void)src; (void)action;
}

static const struct wl_data_source_listener data_source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_drop_performed,
    .dnd_finished = source_finished,
    .action = source_action,
};

static void primary_send(void *data, struct zwp_primary_selection_source_v1 *src, const char *mime, int32_t fd)
{
    struct offered *o = data;
    if (o->source == src && (mime_bit(mime) & o->kind)) {
        if (!accept_probe(o, fd)) queue_write(fd, o->blob);
    } else close(fd);
}

static void primary_cancelled(void *data, struct zwp_primary_selection_source_v1 *src)
{
    struct offered *o = data;
    if (o->source == src) offered_clear(o);
}

static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = primary_send,
    .cancelled = primary_cancelled,
};

/* --- What others offer: each offer keeps the set of its MIME types --- */

static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime)
{
    (void)offer;
    unsigned *mimes = data;
    if (mimes) *mimes |= mime_bit(mime);
}

static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
    (void)data; (void)offer; (void)actions;
}

static void offer_action(void *data, struct wl_data_offer *offer, uint32_t action)
{
    (void)data; (void)offer; (void)action;
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = offer_mime,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void dd_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
    (void)data; (void)dev;
    wl_data_offer_add_listener(id, &data_offer_listener, calloc(1, sizeof(unsigned)));
}

/* These windows are not drop targets yet: a drag over them is refused, so
 * dropping there (also our own drag brought back) cancels it. */
static void dd_enter(void *data, struct wl_data_device *dev, uint32_t serial,
                     struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id)
{
    (void)data; (void)dev; (void)surface; (void)x; (void)y;
    if (ds.dnd_offer != id) data_offer_drop(&ds.dnd_offer);
    ds.dnd_offer = id;
    if (id) wl_data_offer_accept(id, serial, NULL);
}

static void dd_leave(void *data, struct wl_data_device *dev)
{
    (void)data; (void)dev;
    data_offer_drop(&ds.dnd_offer);
}

static void dd_motion(void *data, struct wl_data_device *dev, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)dev; (void)time; (void)x; (void)y;
}

static void dd_drop(void *data, struct wl_data_device *dev)
{
    (void)data; (void)dev;
    data_offer_drop(&ds.dnd_offer);
}

static void dd_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
    (void)data; (void)dev;
    if (ds.offer != id) data_offer_drop(&ds.offer);
    ds.offer = id;
    selection_observed(CLIPBOARD_REGULAR);
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

static void primary_offer_mime(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime)
{
    (void)offer;
    unsigned *mimes = data;
    if (mimes) *mimes |= mime_bit(mime);
}

static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    .offer = primary_offer_mime,
};

static void psd_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
                           struct zwp_primary_selection_offer_v1 *offer)
{
    (void)data; (void)dev;
    zwp_primary_selection_offer_v1_add_listener(offer, &primary_offer_listener, calloc(1, sizeof(unsigned)));
}

static void psd_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
                          struct zwp_primary_selection_offer_v1 *offer)
{
    (void)data; (void)dev;
    if (ds.primary_offer != offer) primary_offer_drop(&ds.primary_offer);
    ds.primary_offer = offer;
    selection_observed(CLIPBOARD_PRIMARY);
}

static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = psd_data_offer,
    .selection = psd_selection,
};

/* --- Public Module Functions --- */

void wl_data_global(struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (!strcmp(interface, wl_data_device_manager_interface.name) && !ds.manager) {
        ds.manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    } else if (!strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) && !ds.primary_manager) {
        ds.primary_manager = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    }
}

static void devices_release(void)
{
    data_offer_drop(&ds.offer);
    data_offer_drop(&ds.dnd_offer);
    primary_offer_drop(&ds.primary_offer);
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        offered_clear(&ds.slots[i][0]);
        offered_clear(&ds.slots[i][1]);
        ds.observed[i] = 0;
        ds.observed_revision[i]++;
    }
    offered_clear(&ds.drag);
    ds.drag_active = 0;
    if (ds.device) {
        if (wl_proxy_get_version((struct wl_proxy *)ds.device) >= 2) wl_data_device_release(ds.device);
        else wl_data_device_destroy(ds.device);
        ds.device = NULL;
    }
    if (ds.primary_device) {
        zwp_primary_selection_device_v1_destroy(ds.primary_device);
        ds.primary_device = NULL;
    }
}

void wl_data_init(struct wl_display *display, struct wl_seat *seat, struct wl_surface *surface)
{
    if (ds.seat && ds.seat != seat) devices_release(); /* another seat: new devices */
    ds.display = display;
    ds.seat = seat;
    ds.surface = surface;
    if (ds.manager && seat && !ds.device) {
        ds.device = wl_data_device_manager_get_data_device(ds.manager, seat);
        wl_data_device_add_listener(ds.device, &data_device_listener, NULL);
    }
    if (ds.primary_manager && seat && !ds.primary_device) {
        ds.primary_device = zwp_primary_selection_device_manager_v1_get_device(ds.primary_manager, seat);
        zwp_primary_selection_device_v1_add_listener(ds.primary_device, &primary_device_listener, NULL);
    }
}

void wl_data_close(void)
{
    while (ds.nwrites > 0) write_end(0);
    devices_release();
    if (ds.manager) wl_data_device_manager_destroy(ds.manager);
    if (ds.primary_manager) zwp_primary_selection_device_manager_v1_destroy(ds.primary_manager);
    memset(&ds, 0, sizeof ds);
}

int wl_data_active(void)
{
    return ds.display && ds.device;
}

int wl_data_poll(struct pollfd *out, size_t max)
{
    size_t n = 0;
    for (; n < ds.nwrites && n < max; n++) {
        out[n] = (struct pollfd){ .fd = ds.writes[n].fd, .events = POLLOUT };
    }
    return (int)n;
}

void wl_data_dispatch(const struct pollfd *fds, size_t count)
{
    int64_t now = now_ms();
    for (size_t i = 0; i < ds.nwrites;) {
        struct pending *w = &ds.writes[i];
        short ready = 0;
        for (size_t j = 0; j < count; j++) {
            if (fds[j].fd == w->fd) ready |= fds[j].revents;
        }
        int end = now >= w->until || (ready & (POLLERR | POLLNVAL));
        while (!end && (ready & POLLOUT) && w->done < w->blob->size) {
            ssize_t n = write_quiet(w->fd, w->blob->data + w->done, w->blob->size - w->done);
            if (n > 0) w->done += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else end = 1;
        }
        if (w->done >= w->blob->size) end = 1;
        if (end) write_end(i); /* the last one moves into i */
        else i++;
    }
}

int wl_data_timeout(void)
{
    if (ds.nwrites == 0) return -1;
    int64_t now = now_ms(), soonest = INT64_MAX;
    for (size_t i = 0; i < ds.nwrites; i++) {
        if (ds.writes[i].until < soonest) soonest = ds.writes[i].until;
    }
    return soonest <= now ? 0 : soonest - now > INT_MAX ? INT_MAX : (int)(soonest - now);
}

/* The later of the last keyboard and pointer serials the compositor gave
 * this window: set_selection is refused with any other. */
static uint32_t input_serial(void)
{
    uint32_t k = wl_backend_keyboard_serial(), b = wl_backend_button_serial();
    if (!k || !b) return k ? k : b;
    return (int32_t)(k - b) > 0 ? k : b;
}

static unsigned kind_of(const char *mime)
{
    unsigned bit = mime_bit(mime ? mime : "text/plain;charset=utf-8");
    return bit & M_ANY_TEXT ? M_ANY_TEXT : bit;
}

static void sync_done(void *data, struct wl_callback *cb, uint32_t serial)
{
    (void)cb; (void)serial;
    *(int *)data = 1;
}

/* Sync orders the replies; it does not acknowledge set_selection. Dispatch
 * while waiting so clipboard readers and our own source can make progress. */
static int selection_sync_until(struct wl_display *display, int64_t until)
{
    static const struct wl_callback_listener listener = { .done = sync_done };
    struct wl_callback *cb = wl_display_sync(display);
    if (!cb) return CLIPBOARD_FAILED;
    int done = 0, status = CLIPBOARD_FAILED;
    wl_callback_add_listener(cb, &listener, &done);
    while (!done) {
        if (wl_display_dispatch_pending(display) < 0) break;
        if (done) break;
        int64_t left = until - now_ms();
        if (left <= 0) { status = CLIPBOARD_BUSY; break; }
        if (wl_display_prepare_read(display) < 0) continue;
        struct pollfd fds[1 + WRITES_MAX] = { { .fd = wl_display_get_fd(display), .events = POLLIN } };
        if (wl_display_flush(display) < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                wl_display_cancel_read(display);
                break;
            }
            fds[0].events |= POLLOUT;
        }
        int n = wl_data_poll(fds + 1, WRITES_MAX);
        int r = poll(fds, (nfds_t)(1 + n), (int)left);
        int error = errno;
        if (r > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (wl_display_read_events(display) < 0) break;
        } else {
            wl_display_cancel_read(display);
        }
        if (r < 0 && error == EINTR) continue;
        if (r < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!r) { status = CLIPBOARD_BUSY; break; }
        wl_data_dispatch(fds + 1, (size_t)n);
    }
    wl_callback_destroy(cb);
    return done ? CLIPBOARD_OK : status;
}

static int selection_focused(unsigned focus)
{
    return wl_backend_keyboard_focused() && wl_backend_keyboard_focus_generation() == focus;
}

static int selection_ready(int which, unsigned focus)
{
    return selection_focused(focus) &&
           (which == CLIPBOARD_PRIMARY ? ds.primary_device != NULL : ds.device != NULL);
}

static int selection_known(int which, unsigned focus)
{
    return ds.observed[which] && ds.observed_focus[which] == focus;
}

static void *selection_offer(int which)
{
    return which == CLIPBOARD_PRIMARY ? (void *)ds.primary_offer : ds.offer;
}

static const char *selection_mime(int which, void *offer, unsigned want)
{
    if (!offer) return NULL;
    const unsigned *mimes = which == CLIPBOARD_PRIMARY ? zwp_primary_selection_offer_v1_get_user_data(offer) :
                                                       wl_data_offer_get_user_data(offer);
    for (size_t i = 0; mimes && i < NMIMES; i++) {
        if (mime_names[i].bit & want & *mimes) return mime_names[i].name;
    }
    return NULL;
}

static void selection_receive(int which, void *offer, const char *name, int fd)
{
    if (which == CLIPBOARD_PRIMARY) zwp_primary_selection_offer_v1_receive(offer, name, fd);
    else wl_data_offer_receive(offer, name, fd);
}

static void selection_request(int which, void *source, uint32_t serial)
{
    if (which == CLIPBOARD_PRIMARY) zwp_primary_selection_device_v1_set_selection(ds.primary_device, source, serial);
    else wl_data_device_set_selection(ds.device, source, serial);
}

static int selection_publish(int which, struct offered *o)
{
    o->primary = which == CLIPBOARD_PRIMARY;
    if (o->primary) {
        o->source = zwp_primary_selection_device_manager_v1_create_source(ds.primary_manager);
        if (!o->source) return -1;
        zwp_primary_selection_source_v1_add_listener(o->source, &primary_source_listener, o);
    } else {
        o->source = wl_data_device_manager_create_data_source(ds.manager);
        if (!o->source) return -1;
        wl_data_source_add_listener(o->source, &data_source_listener, o);
    }
    for (size_t i = 0; i < NMIMES; i++) {
        if (!(mime_names[i].bit & o->kind)) continue;
        if (o->primary) zwp_primary_selection_source_v1_offer(o->source, mime_names[i].name);
        else wl_data_source_offer(o->source, mime_names[i].name);
    }
    return 0;
}

static int selection_confirm(int which, struct offered *o, int64_t until)
{
    void *offer = selection_offer(which);
    const char *name = selection_mime(which, offer, o->kind);
    if (!name) return CLIPBOARD_FAILED;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) return CLIPBOARD_FAILED;
    struct selection_probe probe = { .source = o };
    int status = CLIPBOARD_FAILED;
    if (fstat(fds[1], &probe.pipe) == 0) {
        unsigned revision = ds.observed_revision[which];
        ds.probe = &probe;
        selection_receive(which, offer, name, fds[1]);
        status = selection_sync_until(ds.display, until);
        ds.probe = NULL;
        if (status == CLIPBOARD_OK && (!probe.matched || !o->source || revision != ds.observed_revision[which]))
            status = CLIPBOARD_FAILED;
    }
    close(fds[1]);
    close(fds[0]);
    return status;
}

static int selection_change(int which, unsigned kind, const void *data, size_t size)
{
    unsigned focus = wl_backend_keyboard_focus_generation();
    uint32_t serial = input_serial();
    if (!ds.display || !serial || !selection_ready(which, focus)) return CLIPBOARD_FAILED;
    if (ds.selection_busy) return CLIPBOARD_BUSY;
    ds.selection_busy = 1;
    int64_t until = now_ms() + CLIPBOARD_TIMEOUT_MS;
    int status = selection_sync_until(ds.display, until);
    struct offered *o = NULL;
    if (status != CLIPBOARD_OK) goto out;
    status = CLIPBOARD_FAILED;
    if (!selection_ready(which, focus)) goto out;
    if (!kind && selection_known(which, focus) && !selection_offer(which)) {
        status = CLIPBOARD_OK; /* Already empty while focused. */
        goto out;
    }
    if (kind) {
        o = &ds.slots[which][ds.own[which] == &ds.slots[which][0]];
        o->blob = blob_new(data, size);
        o->kind = kind;
        if (!o->blob || selection_publish(which, o) < 0) goto out;
    }
    unsigned revision = ds.observed_revision[which];
    selection_request(which, o ? o->source : NULL, serial);
    status = selection_sync_until(ds.display, until);
    if (status != CLIPBOARD_OK) goto out;
    status = CLIPBOARD_FAILED;
    if (!selection_ready(which, focus) || !selection_known(which, focus) || revision == ds.observed_revision[which])
        goto out;
    if (o) {
        if (!o->source) goto out;
        status = selection_confirm(which, o, until);
        if (status != CLIPBOARD_OK) goto out;
        if (!selection_ready(which, focus)) { status = CLIPBOARD_FAILED; goto out; }
        /* The compositor cancelled the old source when it accepted the new one. */
        if (ds.own[which]) offered_clear(ds.own[which]);
        ds.own[which] = o;
        o = NULL;
    } else if (!selection_offer(which)) {
        if (ds.own[which]) offered_clear(ds.own[which]);
        status = CLIPBOARD_OK;
    }

out:
    if (o) offered_clear(o);
    ds.selection_busy = 0;
    return status;
}

int wl_data_clipboard_set(int primary, const char *mime, const void *data, size_t size)
{
    unsigned kind = kind_of(mime);
    return kind ? selection_change(primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR, kind, data, size) : CLIPBOARD_FAILED;
}

int wl_data_clipboard_clear(int primary)
{
    return selection_change(primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR, 0, NULL, 0);
}

/* Bytes as the host clipboard service returns them: a sealed memfd. */
static int sealed(const char *data, size_t size, int *fd)
{
    int m = memfd_create("clipboard-data", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (m < 0) return CLIPBOARD_FAILED;
    for (size_t done = 0; done < size;) {
        ssize_t n = write(m, data + done, size - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(m); return CLIPBOARD_FAILED; }
    }
    if (fcntl(m, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0 ||
        lseek(m, 0, SEEK_SET) < 0) {
        close(m);
        return CLIPBOARD_FAILED;
    }
    *fd = m;
    return CLIPBOARD_OK;
}

/* Another client's bytes through a pipe it writes itself: bounded by the
 * host service's deadline and size limit, never waiting for our own loop. */
static int read_offer(int in, int *fd, size_t *size)
{
    size_t cap = 0, len = 0;
    char *buf = NULL;
    int64_t until = now_ms() + CLIPBOARD_TIMEOUT_MS;
    int status = CLIPBOARD_OK;
    for (;;) {
        int64_t left = until - now_ms();
        struct pollfd p = { .fd = in, .events = POLLIN };
        int r = left > 0 ? poll(&p, 1, (int)left) : 0;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { status = r == 0 ? CLIPBOARD_BUSY : CLIPBOARD_FAILED; break; }
        if (len == cap) {
            size_t next = cap ? cap * 2 : 65536;
            char *grown = next <= DATA_LIMIT + 1 ? realloc(buf, next) : NULL;
            if (!grown) { status = CLIPBOARD_FAILED; break; }
            buf = grown;
            cap = next;
        }
        ssize_t n = read(in, buf + len, cap - len);
        if (n > 0) {
            len += (size_t)n;
            if (len > DATA_LIMIT) { status = CLIPBOARD_FAILED; break; }
        } else if (n == 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            status = CLIPBOARD_FAILED;
            break;
        }
    }
    close(in);
    if (status == CLIPBOARD_OK) status = sealed(buf ? buf : "", len, fd);
    if (status == CLIPBOARD_OK) *size = len;
    free(buf);
    return status;
}

int wl_data_clipboard_get(int primary, const char *mime, int *out_fd, size_t *out_size)
{
    *out_fd = -1;
    *out_size = 0;
    unsigned want = kind_of(mime);
    int which = primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR;
    if (!ds.display || !want || (primary ? !ds.primary_device : !ds.device)) return CLIPBOARD_FAILED;
    /* Our own selection is read here: asking the compositor for it would
     * wait for this very loop to answer. */
    struct offered *o = ds.own[which];
    if (o) {
        if (!(o->kind & want)) return CLIPBOARD_NO_TEXT;
        int status = sealed(o->blob->data, o->blob->size, out_fd);
        if (status == CLIPBOARD_OK) *out_size = o->blob->size;
        return status;
    }
    void *offer = selection_offer(which);
    if (!offer) return CLIPBOARD_EMPTY;
    const char *name = selection_mime(which, offer, want);
    if (!name) return CLIPBOARD_NO_TEXT;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return CLIPBOARD_FAILED;
    selection_receive(which, offer, name, p[1]);
    close(p[1]);
    wl_display_flush(ds.display);
    return read_offer(p[0], out_fd, out_size);
}

uint32_t wl_data_press_serial(void)
{
    return wl_backend_press_serial();
}

void wl_data_pointer_pressed(void)
{
    if (ds.drag_active && !ds.drag_dropped) {
        offered_clear(&ds.drag);
        ds.drag_finished = 0;
    }
}

static int drag_start(const char *uris, size_t size, const char *key, uint32_t press_serial,
                      void (*ended)(int finished, void *data), void *data)
{
    if (!ds.display || !ds.manager || !ds.device || !ds.surface || ds.drag_active) return -1;
    if (!press_serial || !uris || size == 0) return -1;
    /* The end of a drag with a key must be known: version 3 tells it. */
    if (key && wl_proxy_get_version((struct wl_proxy *)ds.manager) < 3) return -1;
    struct blob *b = blob_new(uris, size), *k = key ? blob_new(key, strlen(key) + 1) : NULL;
    if (!b || (key && !k)) {
        blob_put(b);
        blob_put(k);
        return -1;
    }
    offered_clear(&ds.drag);
    struct wl_data_source *src = wl_data_device_manager_create_data_source(ds.manager);
    ds.drag = (struct offered){ .source = src, .blob = b, .key = k, .kind = M_URI | (k ? M_KEY : 0) };
    wl_data_source_add_listener(src, &data_source_listener, &ds.drag);
    /* The key first, as GTK offers it: a receiver that knows it prefers it. */
    if (k) {
        wl_data_source_offer(src, "application/vnd.portal.filetransfer");
        wl_data_source_offer(src, "application/vnd.portal.files");
    }
    wl_data_source_offer(src, "text/uri-list");
    /* Copy only: the files stay where they are whatever the target does. */
    if (wl_proxy_get_version((struct wl_proxy *)src) >= 3) {
        wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
    ds.drag_active = 1;
    ds.drag_finished = 0;
    ds.drag_ended = ended;
    ds.drag_ended_data = data;
    wl_data_device_start_drag(ds.device, src, ds.surface, NULL, press_serial);
    wl_display_flush(ds.display);
    return 0;
}

int wl_data_drag_start(const char *uris, size_t size, uint32_t press_serial)
{
    return drag_start(uris, size, NULL, press_serial, NULL, NULL);
}

int wl_data_drag_start_key(const char *uris, size_t size, const char *key, uint32_t press_serial,
                           void (*ended)(int finished, void *data), void *data)
{
    return key && key[0] ? drag_start(uris, size, key, press_serial, ended, data) : -1;
}

int wl_data_drag_active(void)
{
    return ds.drag_active;
}

int wl_data_drag_finished(void)
{
    return ds.drag_finished;
}

#else

/* Direct/musl stubs */
void wl_data_global(struct wl_registry *r, uint32_t n, const char *i, uint32_t v) { (void)r;(void)n;(void)i;(void)v; }
void wl_data_init(struct wl_display *d, struct wl_seat *s, struct wl_surface *surf) { (void)d;(void)s;(void)surf; }
void wl_data_close(void) {}
int wl_data_active(void) { return 0; }
int wl_data_poll(struct pollfd *o, size_t m) { (void)o; (void)m; return 0; }
void wl_data_dispatch(const struct pollfd *f, size_t c) { (void)f; (void)c; }
int wl_data_timeout(void) { return -1; }
int wl_data_clipboard_get(int p, const char *m, int *f, size_t *s) { (void)p;(void)m;*f=-1;*s=0; return CLIPBOARD_FAILED; }
int wl_data_clipboard_set(int p, const char *m, const void *d, size_t s) { (void)p;(void)m;(void)d;(void)s; return CLIPBOARD_FAILED; }
int wl_data_clipboard_clear(int p) { (void)p; return CLIPBOARD_FAILED; }
uint32_t wl_data_press_serial(void) { return 0; }
int wl_data_drag_start(const char *u, size_t s, uint32_t ser) { (void)u;(void)s;(void)ser; return -1; }
int wl_data_drag_start_key(const char *u, size_t s, const char *k, uint32_t ser, void (*e)(int, void *), void *d)
{ (void)u;(void)s;(void)k;(void)ser;(void)e;(void)d; return -1; }
int wl_data_drag_active(void) { return 0; }
int wl_data_drag_finished(void) { return 0; }

#endif
