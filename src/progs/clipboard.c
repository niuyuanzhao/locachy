/* The host clipboard: one current selection and one primary selection in
 * RAM, outside every app and display cgroup. Wayland may go away; the local
 * socket continues to serve term. No history, no content files or logs.
 *
 * clipboard [--serve] [--limit BYTES] [--timeout MS]
 * clipboard get|set|clear [--primary]
 * clipboard get|set --png        an image (image/png) in the regular selection
 * clipboard sync | check GENERATION | freeze CGROUP | status
 * LOCACHY_CLIPBOARD_SOCKET and WAYLAND_DISPLAY select private test instances.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "clipboard.h"
#include "ext-data-control-v1-client-protocol.h"

static size_t limit = 64 * 1024 * 1024;
static int transfer_ms = 2000;
static volatile sig_atomic_t stopping;
static uint64_t generation;
static int64_t retry_at;
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *manager;
static struct ext_data_control_device_v1 *device;
static int disconnected, want_flush;

static int64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

struct blob {
    int fd;
    size_t size, refs;
};
struct format {
    char *mime;
    struct blob *blob;
};
struct contents {
    struct format *formats;
    size_t count;
};
struct offer {
    struct ext_data_control_offer_v1 *object;
    struct contents data;
    size_t index, total;
    int pipe, data_fd;
    int64_t until;
};
struct source {
    struct ext_data_control_source_v1 *object;
    struct contents data;
    char marker[96];
    int selection;
    struct source *next;
};
struct selection {
    struct contents saved;
    struct offer *offer;
    struct source *source;
    int initial, failed, needs_publish, present, publishing;
    uint64_t revision, sent_revision;
    struct wl_callback *published;
};
static struct selection selections[CLIPBOARD_SELECTIONS];
static struct source *sources;
struct sending {
    int fd;
    size_t offset;
    struct blob *blob;
    int64_t until;
    struct sending *next;
};
static struct sending *sends;
struct client {
    int fd, requested, syncing, synced;
    int64_t until;
    uint64_t snapshot;
    struct clipboard_message message;
    struct wl_callback *callback;
    struct client *next;
};
static struct client *clients;

static void blob_drop(struct blob *b)
{
    if (b && --b->refs == 0) {
        close(b->fd);
        free(b);
    }
}

static void contents_drop(struct contents *c)
{
    for (size_t i = 0; i < c->count; i++) {
        free(c->formats[i].mime);
        blob_drop(c->formats[i].blob);
    }
    free(c->formats);
    memset(c, 0, sizeof *c);
}

static int add_format(struct contents *c, const char *mime, struct blob *b)
{
    if (c->count >= SIZE_MAX / sizeof *c->formats) return -1;
    char *name = strdup(mime);
    struct format *p = name ? realloc(c->formats, (c->count + 1) * sizeof *p) : NULL;
    if (!p) { free(name); return -1; }
    c->formats = p;
    c->formats[c->count++] = (struct format){ name, b };
    if (b) b->refs++;
    return 0;
}

static struct blob *find_format(const struct contents *c, const char *mime)
{
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->formats[i].mime, mime) == 0) return c->formats[i].blob;
    }
    return NULL;
}

static void offer_drop(struct offer *o)
{
    if (!o) return;
    if (o->pipe >= 0) close(o->pipe);
    if (o->data_fd >= 0) close(o->data_fd);
    if (o->object) ext_data_control_offer_v1_destroy(o->object);
    contents_drop(&o->data);
    free(o);
}

static void source_drop(struct source *s)
{
    if (!s) return;
    if (selections[s->selection].source == s) selections[s->selection].source = NULL;
    struct source **p = &sources;
    while (*p && *p != s) p = &(*p)->next;
    if (*p) *p = s->next;
    ext_data_control_source_v1_destroy(s->object);
    contents_drop(&s->data);
    free(s);
}

static void source_send(void *data, struct ext_data_control_source_v1 *object, const char *mime, int fd)
{
    (void)object;
    struct source *s = data;
    struct blob *b = find_format(&s->data, mime);
    struct sending *send = b ? calloc(1, sizeof *send) : NULL;
    if (!send || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        free(send);
        close(fd);
        return;
    }
    b->refs++;
    *send = (struct sending){ .fd = fd, .blob = b, .until = now() + transfer_ms, .next = sends };
    sends = send;
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *object)
{
    (void)object;
    source_drop(data);
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send, .cancelled = source_cancelled,
};

static void publish(int which);

static void published_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial;
    struct selection *sel = data;
    wl_callback_destroy(callback);
    sel->published = NULL;
    sel->publishing = 0;
    if (sel->needs_publish) publish((int)(sel - selections));
}
static const struct wl_callback_listener published_listener = { .done = published_done };

static void publish(int which)
{
    struct selection *sel = &selections[which];
    if (!device || !sel->needs_publish || sel->publishing) return;
    sel->needs_publish = 0;
    sel->publishing = 1;
    sel->sent_revision = sel->revision;
    if (!sel->present) {
        if (which == CLIPBOARD_PRIMARY) ext_data_control_device_v1_set_primary_selection(device, NULL);
        else ext_data_control_device_v1_set_selection(device, NULL);
        sel->published = wl_display_sync(display);
        wl_callback_add_listener(sel->published, &published_listener, sel);
        return;
    }
    struct source *s = calloc(1, sizeof *s);
    if (!s) { sel->failed = 1; sel->publishing = 0; return; }
    s->selection = which;
    for (size_t i = 0; i < sel->saved.count; i++) {
        if (add_format(&s->data, sel->saved.formats[i].mime, sel->saved.formats[i].blob) < 0) {
            contents_drop(&s->data);
            free(s);
            sel->failed = 1;
            sel->publishing = 0;
            return;
        }
    }
    uint64_t nonce[2];
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t got = random >= 0 ? read(random, nonce, sizeof nonce) : -1;
    if (random >= 0) close(random);
    if (got != sizeof nonce) {
        contents_drop(&s->data); free(s); sel->failed = 1; sel->publishing = 0; return;
    }
    snprintf(s->marker, sizeof s->marker, "application/x-locachy-clipboard-%016" PRIx64 "%016" PRIx64,
             nonce[0], nonce[1]);
    s->object = ext_data_control_manager_v1_create_data_source(manager);
    ext_data_control_source_v1_add_listener(s->object, &source_listener, s);
    for (size_t i = 0; i < s->data.count; i++) {
        ext_data_control_source_v1_offer(s->object, s->data.formats[i].mime);
    }
    /* Only an offer from our still-live source with this random marker is
     * ours: a different client replaying an old marker cannot hide its offer. */
    ext_data_control_source_v1_offer(s->object, s->marker);
    s->next = sources;
    sources = s;
    sel->source = s;
    if (which == CLIPBOARD_PRIMARY) ext_data_control_device_v1_set_primary_selection(device, s->object);
    else ext_data_control_device_v1_set_selection(device, s->object);
    sel->published = wl_display_sync(display);
    wl_callback_add_listener(sel->published, &published_listener, sel);
}

static void receive_next(int which)
{
    struct selection *sel = &selections[which];
    struct offer *o = sel->offer;
    if (!o) return;
    if (o->index == o->data.count) {
        contents_drop(&sel->saved);
        sel->saved = o->data;
        memset(&o->data, 0, sizeof o->data);
        sel->offer = NULL;
        sel->present = 1;
        sel->failed = 0;
        sel->revision++;
        offer_drop(o);
        /* Do not steal primary during a drag: sync takes it before freeze. */
        if (which == CLIPBOARD_REGULAR) {
            sel->needs_publish = 1;
            publish(which);
        }
        return;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) goto failed;
    o->data_fd = memfd_create("clipboard-format", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (o->data_fd < 0) { close(pipefd[0]); close(pipefd[1]); goto failed; }
    o->pipe = pipefd[0];
    /* The source owns the write end; leave its ordinary blocking semantics
     * intact. Our read end is nonblocking. */
    fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL) & ~O_NONBLOCK);
    ext_data_control_offer_v1_receive(o->object, o->data.formats[o->index].mime, pipefd[1]);
    close(pipefd[1]);
    return;
failed:
    sel->offer = NULL;
    sel->failed = 1;
    offer_drop(o);
    fprintf(stderr, "clipboard: cannot allocate a selection transfer\n");
}

static void read_offer(int which)
{
    struct selection *sel = &selections[which];
    struct offer *o = sel->offer;
    if (!o || o->pipe < 0) return;
    char buf[65536];
    /* A busy source must not starve IPC or the other selection. */
    for (int step = 0; step < 16; step++) {
        ssize_t n = read(o->pipe, buf, sizeof buf);
        if (n > 0) {
            if ((size_t)n > limit - o->total || write(o->data_fd, buf, (size_t)n) != n) goto failed;
            o->total += (size_t)n;
        } else if (n == 0) {
            off_t size = lseek(o->data_fd, 0, SEEK_END);
            struct blob *b = size >= 0 ? calloc(1, sizeof *b) : NULL;
            if (!b || fcntl(o->data_fd, F_ADD_SEALS,
                           F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
                free(b); goto failed;
            }
            *b = (struct blob){ .fd = o->data_fd, .size = (size_t)size, .refs = 1 };
            o->data_fd = -1;
            o->data.formats[o->index++].blob = b;
            close(o->pipe);
            o->pipe = -1;
            receive_next(which);
            return;
        } else if (errno == EAGAIN) return;
        else if (errno != EINTR) goto failed;
    }
    return;
failed:
    sel->offer = NULL;
    sel->failed = 1;
    offer_drop(o);
    fprintf(stderr, "clipboard: selection transfer failed or exceeded its memory budget\n");
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *object, const char *mime)
{
    (void)object;
    struct offer *o = data;
    if (strcmp(mime, "SAVE_TARGETS") == 0) return;
    for (size_t i = 0; i < o->data.count; i++) {
        if (strcmp(mime, o->data.formats[i].mime) == 0) return;
    }
    size_t cost = strlen(mime) + 1 + sizeof(struct format);
    if (cost > limit - o->total || add_format(&o->data, mime, NULL) < 0) {
        o->until = 0;
    } else {
        o->total += cost;
    }
}

static const struct ext_data_control_offer_v1_listener offer_listener = { .offer = offer_mime };

static void device_offer(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *object)
{
    (void)data; (void)d;
    struct offer *o = calloc(1, sizeof *o);
    if (!o) { disconnected = 1; return; }
    o->object = object;
    o->pipe = o->data_fd = -1;
    o->until = now() + transfer_ms;
    ext_data_control_offer_v1_add_listener(object, &offer_listener, o);
}

static void new_selection(int which, struct ext_data_control_offer_v1 *object)
{
    struct selection *sel = &selections[which];
    struct offer *o = object ? ext_data_control_offer_v1_get_user_data(object) : NULL;
    int initial = sel->initial;
    sel->initial = 0;
    if (o && sel->source) {
        for (size_t i = 0; i < o->data.count; i++) {
            if (strcmp(o->data.formats[i].mime, sel->source->marker) == 0) {
                offer_drop(o);
                return;
            }
        }
    }
    /* A local update while Wayland was frozen wins over the stale initial
     * selection. No Wayland response is needed for the local SET reply. */
    if (initial && sel->needs_publish) {
        offer_drop(o);
        publish(which);
        return;
    }
    if (!o && initial && sel->present) {
        sel->needs_publish = 1;
        publish(which);
        return;
    }
    if (sel->needs_publish && sel->publishing && sel->revision != sel->sent_revision) {
        offer_drop(o);
        return;
    }
    offer_drop(sel->offer);
    sel->offer = NULL;
    sel->failed = 0;
    generation++;
    sel->revision++;
    if (!o) {
        contents_drop(&sel->saved);
        sel->present = 0;
        return;
    }
    sel->offer = o;
    o->until = o->until == 0 ? 0 : now() + transfer_ms;
    receive_next(which);
}

static void device_selection(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *o)
{ (void)data; (void)d; new_selection(CLIPBOARD_REGULAR, o); }
static void device_primary(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *o)
{ (void)data; (void)d; new_selection(CLIPBOARD_PRIMARY, o); }
static void device_finished(void *data, struct ext_data_control_device_v1 *d)
{ (void)data; (void)d; disconnected = 1; }
static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_offer, .selection = device_selection, .finished = device_finished,
    .primary_selection = device_primary,
};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!seat && strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    } else if (!manager && strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
    }
    if (seat && manager && !device) {
        for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) selections[i].initial = 1;
        device = ext_data_control_manager_v1_get_data_device(manager, seat);
        ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    }
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{ (void)data; (void)r; (void)name; }
static const struct wl_registry_listener registry_listener = { global, global_remove };

static void drop_display(void)
{
    for (struct client *c = clients; c; c = c->next) {
        if (c->callback) { wl_callback_destroy(c->callback); c->callback = NULL; }
        c->syncing = 0;
    }
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        if (selections[i].offer) selections[i].failed = 1;
        offer_drop(selections[i].offer);
        selections[i].offer = NULL;
        if (selections[i].published) wl_callback_destroy(selections[i].published);
        selections[i].published = NULL;
        selections[i].publishing = 0;
    }
    while (sources) source_drop(sources);
    if (device) ext_data_control_device_v1_destroy(device);
    if (manager) ext_data_control_manager_v1_destroy(manager);
    if (seat) wl_seat_destroy(seat);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
    display = NULL; registry = NULL; seat = NULL; manager = NULL; device = NULL;
    disconnected = want_flush = 0;
    retry_at = now() + 200;
}

static void connect_display(void)
{
    if (display || now() < retry_at) return;
    retry_at = now() + 200;
    const char *runtime = getenv("XDG_RUNTIME_DIR"), *name = getenv("WAYLAND_DISPLAY");
    if (!runtime) return;
    if (!name) name = "wayland-0";
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int n = name[0] == '/' ? snprintf(address.sun_path, sizeof address.sun_path, "%s", name) :
        snprintf(address.sun_path, sizeof address.sun_path, "%s/%s", runtime, name);
    if (n < 0 || n >= (int)sizeof address.sun_path) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) { close(fd); return; }
    display = wl_display_connect_to_fd(fd);
    if (!display) return;
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
}

static int pending(void)
{
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        if (selections[i].failed) return CLIPBOARD_FAILED;
        if (selections[i].offer || selections[i].publishing || (device && selections[i].needs_publish)) return CLIPBOARD_BUSY;
    }
    return CLIPBOARD_OK;
}

static void callback_done(void *data, struct wl_callback *object, uint32_t serial)
{
    (void)serial;
    struct client *c = data;
    wl_callback_destroy(object);
    c->callback = NULL;
    c->synced = 1;
}
static const struct wl_callback_listener callback_listener = { .done = callback_done };

static void client_reply(struct client *c, int status, int data)
{
    c->message.status = (uint32_t)status;
    c->message.version = CLIPBOARD_VERSION;
    char space[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = &c->message, .iov_len = sizeof c->message };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (data >= 0) {
        m.msg_control = space; m.msg_controllen = sizeof space;
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &data, sizeof data);
    }
    /* One small reply; an unread client never stalls the event loop. */
    sendmsg(c->fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(c->fd);
    c->fd = -1;
}

/* The selections as a sync left them, kept (blob references, no second copy
 * of the bytes) for a RESTORE of that sync's generation: a freeze asks for
 * it after its freezer write, when a copy that came in between, or one the
 * freeze cut short, would otherwise stand. Dropped after HOLD_MS. */
#define HOLD_MS 10000
static struct held {
    struct contents saved;
    int present;
    uint64_t revision;
} held[CLIPBOARD_SELECTIONS];
static uint64_t held_generation;
static int64_t held_until;
static int holding;

static void hold_release(void)
{
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) contents_drop(&held[i].saved);
    holding = 0;
}

static void hold(void)
{
    hold_release();
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        struct selection *sel = &selections[i];
        for (size_t f = 0; f < sel->saved.count; f++) {
            if (add_format(&held[i].saved, sel->saved.formats[f].mime, sel->saved.formats[f].blob) < 0) {
                hold_release();
                return;
            }
        }
        held[i].present = sel->present;
        held[i].revision = sel->revision;
    }
    held_generation = generation;
    held_until = now() + HOLD_MS;
    holding = 1;
}

static int restore(uint64_t at)
{
    if (!holding || held_generation != at) return CLIPBOARD_FAILED;
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        struct selection *sel = &selections[i];
        if (sel->revision == held[i].revision && !sel->offer && !sel->failed) continue;
        offer_drop(sel->offer);
        sel->offer = NULL;
        contents_drop(&sel->saved);
        sel->saved = held[i].saved;
        memset(&held[i].saved, 0, sizeof held[i].saved);
        sel->present = held[i].present;
        sel->failed = 0;
        sel->revision++;
        generation++;
        sel->needs_publish = 1;
        publish(i);
    }
    hold_release();
    return CLIPBOARD_OK;
}

static void serve_client(struct client *c)
{
    char space[CMSG_SPACE(sizeof(int) * 8)] = { 0 };
    struct iovec iov = { .iov_base = &c->message, .iov_len = sizeof c->message };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = space, .msg_controllen = sizeof space };
    ssize_t n = recvmsg(c->fd, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    int data = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int fd;
            memcpy(&fd, (char *)CMSG_DATA(h) + i * sizeof fd, sizeof fd);
            if (data < 0) data = fd; else close(fd);
        }
    }
    if (n != sizeof c->message || c->message.version != CLIPBOARD_VERSION ||
        c->message.selection >= CLIPBOARD_SELECTIONS || (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        if (data >= 0) close(data);
        client_reply(c, CLIPBOARD_FAILED, -1);
        return;
    }
    c->requested = 1;
    struct selection *sel = &selections[c->message.selection];
    int status = CLIPBOARD_OK;
    switch (c->message.operation) {
    case CLIPBOARD_GET:
    case CLIPBOARD_GET_PNG: {
        static const char *const text[] = { "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
            "UTF8_STRING", "text/plain", "TEXT", NULL };
        static const char *const png[] = { "image/png", NULL };
        const char *const *types = c->message.operation == CLIPBOARD_GET_PNG ? png : text;
        if (sel->offer) status = CLIPBOARD_BUSY;
        else if (sel->failed) status = CLIPBOARD_FAILED;
        else if (!sel->present) status = CLIPBOARD_EMPTY;
        else {
            struct blob *b = NULL;
            for (int i = 0; types[i] && !b; i++) b = find_format(&sel->saved, types[i]);
            if (!b) status = CLIPBOARD_NO_TEXT;
            else {
                c->message.length = b->size;
                client_reply(c, CLIPBOARD_OK, b->fd);
                goto done;
            }
        }
        break;
    }
    case CLIPBOARD_SET:
    case CLIPBOARD_SET_PNG: {
        struct stat st;
        int seals = data >= 0 ? fcntl(data, F_GET_SEALS) : -1;
        if (data < 0 || fstat(data, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (uint64_t)st.st_size != c->message.length || c->message.length > limit || seals < 0 ||
            (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) !=
                     (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) {
            status = CLIPBOARD_FAILED;
            break;
        }
        struct blob *b = calloc(1, sizeof *b);
        struct contents next = { 0 };
        if (!b) { status = CLIPBOARD_FAILED; break; }
        *b = (struct blob){ .fd = data, .size = (size_t)st.st_size, .refs = 1 };
        data = -1;
        if (c->message.operation == CLIPBOARD_SET_PNG ? add_format(&next, "image/png", b) < 0 :
            (add_format(&next, "text/plain;charset=utf-8", b) < 0 ||
             add_format(&next, "text/plain", b) < 0 || add_format(&next, "UTF8_STRING", b) < 0)) {
            blob_drop(b); contents_drop(&next); status = CLIPBOARD_FAILED; break;
        }
        blob_drop(b);
        offer_drop(sel->offer); sel->offer = NULL;
        contents_drop(&sel->saved); sel->saved = next;
        sel->present = 1; sel->failed = 0; sel->needs_publish = 1;
        sel->revision++;
        generation++;
        publish((int)c->message.selection);
        break;
    }
    case CLIPBOARD_CLEAR:
        offer_drop(sel->offer); sel->offer = NULL;
        contents_drop(&sel->saved);
        sel->present = sel->failed = 0;
        sel->needs_publish = 1;
        sel->revision++;
        generation++;
        publish((int)c->message.selection);
        break;
    case CLIPBOARD_SYNC:
    case CLIPBOARD_CHECK:
        c->snapshot = generation;
        if (display) {
            c->callback = wl_display_sync(display);
            wl_callback_add_listener(c->callback, &callback_listener, c);
            c->syncing = 1;
        } else c->synced = 1;
        goto done;
    case CLIPBOARD_RESTORE:
        status = restore(c->message.generation);
        break;
    case CLIPBOARD_STATUS:
        c->message.generation = generation;
        c->message.length = display && device ? 1 : 0;
        status = pending();
        break;
    default: status = CLIPBOARD_FAILED; break;
    }
    client_reply(c, status, -1);
done:
    if (data >= 0) close(data);
}

static void service_barriers(void)
{
    for (struct client *c = clients; c; c = c->next) {
        if (c->fd < 0 || !c->requested || !c->synced) continue;
        int status = pending();
        if (c->message.operation == CLIPBOARD_CHECK) {
            if (status == CLIPBOARD_BUSY) status = CLIPBOARD_BUSY;
            else if (status == CLIPBOARD_OK && c->message.generation != generation) status = CLIPBOARD_BUSY;
            client_reply(c, status, -1);
            continue;
        }
        if (status == CLIPBOARD_BUSY) continue;
        if (status != CLIPBOARD_OK) { client_reply(c, status, -1); continue; }
        if (device && c->syncing == 1) {
            /* Primary was prefetched without stealing the live selection. */
            for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
                if (selections[i].present && !selections[i].source) {
                    selections[i].needs_publish = 1;
                    publish(i);
                }
            }
            c->synced = 0;
            c->snapshot = generation;
            c->callback = wl_display_sync(display);
            wl_callback_add_listener(c->callback, &callback_listener, c);
            c->syncing = 2;
            continue;
        }
        if (c->snapshot != generation) {
            c->syncing = display ? 1 : 0;
            c->snapshot = generation;
            continue;
        }
        c->message.generation = generation;
        hold();
        client_reply(c, CLIPBOARD_OK, -1);
    }
}

static void accept_client(int listener)
{
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    struct ucred peer;
    socklen_t size = sizeof peer;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || peer.uid != geteuid()) {
        close(fd); return;
    }
    struct client *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return; }
    c->fd = fd; c->until = now() + transfer_ms + 100;
    c->next = clients; clients = c;
}

static void sweep(void)
{
    int64_t t = now();
    if (holding && t >= held_until) hold_release();
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
        struct selection *sel = &selections[i];
        if (sel->offer && t >= sel->offer->until) {
            offer_drop(sel->offer); sel->offer = NULL; sel->failed = 1;
            fprintf(stderr, "clipboard: selection transfer timed out\n");
        }
    }
    for (struct sending **p = &sends; *p;) {
        struct sending *s = *p;
        int done = t >= s->until;
        if (!done) {
            char buf[65536];
            size_t left = s->blob->size - s->offset;
            ssize_t n = left ? pread(s->blob->fd, buf, left < sizeof buf ? left : sizeof buf, (off_t)s->offset) : 0;
            if (n == 0) done = 1;
            else if (n < 0) done = errno != EINTR;
            else {
                ssize_t w = write(s->fd, buf, (size_t)n);
                if (w > 0) {
                    s->offset += (size_t)w;
                    s->until = t + transfer_ms; /* no-progress deadline, not a payload-size deadline */
                }
                else if (w < 0 && errno != EINTR && errno != EAGAIN) done = 1;
            }
        }
        if (done) { *p = s->next; close(s->fd); blob_drop(s->blob); free(s); }
        else p = &s->next;
    }
    for (struct client **p = &clients; *p;) {
        struct client *c = *p;
        if (c->fd >= 0 && t >= c->until) client_reply(c, CLIPBOARD_FAILED, -1);
        if (c->fd < 0) {
            *p = c->next;
            if (c->callback) wl_callback_destroy(c->callback);
            free(c);
        } else p = &c->next;
    }
}

static void stop(int sig) { (void)sig; stopping = 1; }

static int daemon_main(void)
{
    const char *path = getenv("LOCACHY_CLIPBOARD_SOCKET");
    if (!path) path = CLIPBOARD_SOCKET;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (path[0] != '/' || strlen(path) >= sizeof address.sun_path) return 1;
    strcpy(address.sun_path, path);
    mkdir("/run/locachy", 0755);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) return 1;
    unlink(path);
    mode_t mask = umask(0177);
    int bound = bind(listener, (struct sockaddr *)&address, sizeof address);
    umask(mask);
    if (bound < 0 || listen(listener, 16) < 0) {
        perror("clipboard: listen"); close(listener); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop); signal(SIGINT, stop);
    while (!stopping) {
        connect_display();
        if (display && (wl_display_dispatch_pending(display) < 0 || disconnected)) drop_display();
        service_barriers();
        sweep();
        int prepared = 0;
        if (display) {
            while (wl_display_prepare_read(display) != 0) {
                if (wl_display_dispatch_pending(display) < 0 || disconnected) { drop_display(); break; }
            }
            if (display) {
                prepared = 1;
                int flushed = wl_display_flush(display);
                want_flush = flushed < 0 && errno == EAGAIN;
                if (flushed < 0 && !want_flush) {
                    wl_display_cancel_read(display);
                    prepared = 0;
                    drop_display();
                }
            }
        }
        size_t n = 4;
        for (struct client *c = clients; c; c = c->next) n++;
        for (struct sending *s = sends; s; s = s->next) n++;
        struct pollfd *p = calloc(n, sizeof *p);
        if (!p) {
            if (prepared) wl_display_cancel_read(display);
            break;
        }
        p[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
        p[1] = (struct pollfd){ .fd = display ? wl_display_get_fd(display) : -1,
            .events = POLLIN | (want_flush ? POLLOUT : 0) };
        for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) {
            p[2 + i] = (struct pollfd){ .fd = selections[i].offer ? selections[i].offer->pipe : -1, .events = POLLIN };
        }
        size_t i = 4;
        for (struct client *c = clients; c; c = c->next) p[i++] = (struct pollfd){ .fd = c->fd,
            .events = c->requested ? 0 : POLLIN };
        for (struct sending *s = sends; s; s = s->next) p[i++] = (struct pollfd){ .fd = s->fd, .events = POLLOUT };
        int ready = poll(p, (nfds_t)n, 25);
        if (prepared) {
            if (ready > 0 && (p[1].revents & POLLIN)) {
                if (wl_display_read_events(display) < 0) drop_display();
                else if (wl_display_dispatch_pending(display) < 0 || disconnected) drop_display();
            } else {
                wl_display_cancel_read(display);
                if (ready > 0 && (p[1].revents & (POLLHUP | POLLERR))) drop_display();
            }
        }
        if (ready > 0) {
            /* Dispatch can replace offers: never apply old readiness to a
             * new pipe. Read pipes nonblocking only after dispatch. */
            for (int j = 0; j < CLIPBOARD_SELECTIONS; j++) read_offer(j);
            i = 4;
            for (struct client *c = clients; c; c = c->next, i++) {
                if (!c->requested && p[i].revents) serve_client(c);
            }
            if (p[0].revents & POLLIN) accept_client(listener);
        }
        free(p);
    }
    drop_display();
    while (clients) {
        struct client *c = clients; clients = c->next;
        if (c->fd >= 0) close(c->fd);
        free(c);
    }
    while (sends) {
        struct sending *s = sends; sends = s->next;
        close(s->fd); blob_drop(s->blob); free(s);
    }
    for (int i = 0; i < CLIPBOARD_SELECTIONS; i++) contents_drop(&selections[i].saved);
    close(listener); unlink(path);
    return 0;
}

static int cli(int argc, char **argv)
{
    int primary = argc > 2 && strcmp(argv[2], "--primary") == 0, status;
    int png = argc > 2 && strcmp(argv[2], "--png") == 0;
    if (strcmp(argv[1], "set") == 0) {
        char *data = NULL, buf[65536];
        size_t size = 0;
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof buf)) != 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 || (size_t)n > limit - size) { free(data); return 1; }
            char *p = realloc(data, size + (size_t)n);
            if (!p) { free(data); return 1; }
            data = p; memcpy(data + size, buf, (size_t)n); size += (size_t)n;
        }
        status = png ? clipboard_set_png(data, size) : clipboard_set(primary, data, size);
        free(data);
    } else if (strcmp(argv[1], "get") == 0) {
        int fd;
        size_t size;
        status = png ? clipboard_get_png(&fd, &size) : clipboard_get(primary, &fd, &size);
        if (status == CLIPBOARD_OK) {
            char buf[65536];
            size_t offset = 0;
            while (offset < size) {
                ssize_t n = pread(fd, buf, size - offset < sizeof buf ? size - offset : sizeof buf, (off_t)offset);
                if (n <= 0) { close(fd); return 1; }
                size_t sent = 0;
                while (sent < (size_t)n) {
                    ssize_t w = write(STDOUT_FILENO, buf + sent, (size_t)n - sent);
                    if (w > 0) sent += (size_t)w;
                    else if (w < 0 && errno == EINTR) continue;
                    else { close(fd); return 1; }
                }
                offset += (size_t)n;
            }
            close(fd);
        }
    } else if (strcmp(argv[1], "clear") == 0) status = clipboard_clear(primary);
    else if (strcmp(argv[1], "sync") == 0) {
        uint64_t token;
        status = clipboard_sync(&token);
        if (status == CLIPBOARD_OK) printf("%" PRIu64 "\n", token);
    } else if (strcmp(argv[1], "check") == 0 && argc == 3) {
        char *end;
        uint64_t token = strtoull(argv[2], &end, 10);
        if (!argv[2][0] || *end) return 1;
        status = clipboard_check(token);
    } else if (strcmp(argv[1], "freeze") == 0 && argc == 3) {
        status = clipboard_freeze(argv[2]);
    } else if (strcmp(argv[1], "status") == 0) {
        struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = CLIPBOARD_STATUS };
        int fd = -1;
        status = clipboard_request(&m, &fd, CLIPBOARD_TIMEOUT_MS);
        if (fd >= 0) close(fd);
        printf("generation=%" PRIu64 " wayland=%s state=%s\n", m.generation, m.length ? "connected" : "disconnected",
               clipboard_error(status));
    } else return 2;
    if (status == CLIPBOARD_NO_TEXT && png) fprintf(stderr, "clipboard: clipboard has no image\n");
    else if (status != CLIPBOARD_OK) fprintf(stderr, "clipboard: %s\n", clipboard_error(status));
    return status == CLIPBOARD_OK ? 0 : status;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] != '-') return cli(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serve") == 0) continue;
        char *end;
        if (++i >= argc) return 2;
        unsigned long long n = strtoull(argv[i], &end, 10);
        if (!argv[i][0] || *end || !n) return 2;
        if (strcmp(argv[i - 1], "--limit") == 0 && n <= SIZE_MAX) limit = (size_t)n;
        else if (strcmp(argv[i - 1], "--timeout") == 0 && n < CLIPBOARD_TIMEOUT_MS - 100) transfer_ms = (int)n;
        else return 2;
    }
    return daemon_main();
}
