/* SPDX-License-Identifier: GPL-2.0-only
 * The compositor is the freeze linearization point, not a client's sync.
 * begin gates one cgroup's ordinary/primary selection requests while its
 * current source still runs to answer the clipboard manager's reads.
 * commit keeps the gate until the cgroup is actually thawed.
 *
 * Root-only SOCK_SEQPACKET, one directory fd with "begin", then "commit".
 * No clipboard contents cross this channel, no app_id guessing, no private
 * wlroots structures. A disconnected/uncommitted transaction is rolled back.
 */
#define _GNU_SOURCE
#include "locachy-clipboard.h"
#include "labwc.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

struct tracked_client {
    struct wl_client *client;
    pid_t pid;
    int pidfd, cgroup;
    struct wl_listener destroy;
    struct wl_list link;
};
struct guard {
    int client, directory, events, committed, watch, committing;
    struct wl_event_source *socket_source, *timer;
    struct wl_event_source *completion;
    struct wl_list link;
};
static struct wl_list tracked, guards;
static struct wl_listener created;
static struct wl_protocol_logger *logger;
static struct wl_event_source *listener_source;
static int listener = -1, watches = -1;
static struct wl_event_source *watch_source;
static struct wl_client *sender;
static const char *request_interface, *request_name;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static int read_number(int directory, const char *name)
{
    char text[64];
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return -1;
    text[n] = 0;
    return atoi(text);
}

static int frozen(struct guard *g)
{
    char text[256];
    ssize_t n = pread(g->events, text, sizeof text - 1, 0);
    if (n <= 0) return -1;
    text[n] = 0;
    const char *s = strstr(text, "frozen ");
    return s ? atoi(s + 7) : -1;
}

static int same_directory(int a, int b)
{
    struct stat x, y;
    return a >= 0 && b >= 0 && fstat(a, &x) == 0 && fstat(b, &y) == 0 &&
        x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

static int beneath(int child, int parent)
{
    int fd = fcntl(child, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return 0;
    for (;;) {
        if (same_directory(fd, parent)) { close(fd); return 1; }
        int up = openat(fd, "..", O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (up < 0 || same_directory(fd, up)) {
            if (up >= 0) close(up);
            close(fd);
            return 0;
        }
        close(fd);
        fd = up;
    }
}

static int process_cgroup(pid_t pid)
{
    char path[64], *line = NULL;
    size_t size = 0;
    snprintf(path, sizeof path, "/proc/%ld/cgroup", (long)pid);
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int fd = -1;
    while (getline(&line, &size, f) >= 0) {
        if (strncmp(line, "0::/", 4) != 0) continue;
        line[strcspn(line, "\r\n")] = 0;
        char *full;
        if (asprintf(&full, "/sys/fs/cgroup%s", line + 3) >= 0) {
            fd = open(full, O_PATH | O_DIRECTORY | O_CLOEXEC);
            free(full);
        }
        break;
    }
    free(line);
    fclose(f);
    return fd;
}

static void client_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct tracked_client *c = wl_container_of(l, c, destroy);
    if (sender == c->client) sender = NULL;
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->link);
    if (c->pidfd >= 0) close(c->pidfd);
    if (c->cgroup >= 0) close(c->cgroup);
    free(c);
}

static void client_created(struct wl_listener *l, void *data)
{
    (void)l;
    struct tracked_client *c = calloc(1, sizeof *c);
    if (!c) { wl_client_post_no_memory(data); return; }
    c->client = data;
    wl_client_get_credentials(c->client, &c->pid, NULL, NULL);
    c->pidfd = c->cgroup = -1;
    socklen_t size = sizeof c->pidfd;
    if (getsockopt(wl_client_get_fd(c->client), SOL_SOCKET, SO_PEERPIDFD, &c->pidfd, &size) == 0) {
        struct pollfd p = { .fd = c->pidfd, .events = POLLIN };
        if (poll(&p, 1, 0) == 0) {
            int directory = process_cgroup(c->pid);
            p.revents = 0;
            if (poll(&p, 1, 0) == 0) c->cgroup = directory;
            else if (directory >= 0) close(directory);
        }
    }
    c->destroy.notify = client_destroy;
    wl_client_add_destroy_listener(c->client, &c->destroy);
    wl_list_insert(&tracked, &c->link);
}

static void protocol_request(void *data, enum wl_protocol_logger_type direction,
    const struct wl_protocol_logger_message *message)
{
    (void)data;
    if (direction != WL_PROTOCOL_LOGGER_REQUEST) return;
    sender = wl_resource_get_client(message->resource);
    request_interface = wl_resource_get_class(message->resource);
    request_name = message->message->name;
}

static void guard_drop(struct guard *g, int rollback)
{
    /* If the helper died after writing freeze but before commit, do not
     * leave its source frozen behind an aborted transaction. */
    if (rollback && g->directory >= 0) {
        int fd = openat(g->directory, "cgroup.freeze", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) { if (write(fd, "0", 1) < 0) { } close(fd); }
    }
    if (g->socket_source) wl_event_source_remove(g->socket_source);
    if (g->completion) wl_event_source_remove(g->completion);
    if (g->timer) wl_event_source_remove(g->timer);
    if (g->watch >= 0 && watches >= 0) inotify_rm_watch(watches, g->watch);
    if (g->client >= 0) close(g->client);
    if (g->events >= 0) close(g->events);
    if (g->directory >= 0) close(g->directory);
    wl_list_remove(&g->link);
    free(g);
}

static int deadline(void *data)
{
    struct guard *g = data;
    guard_drop(g, !g->committed);
    return 0;
}

static int changed(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    char events[8192];
    while (read(fd, events, sizeof events) > 0) { }
    struct guard *g, *tmp;
    wl_list_for_each_safe(g, tmp, &guards, link) {
        if (g->committed && read_number(g->directory, "cgroup.freeze") != 1) guard_drop(g, 0);
    }
    return 0;
}

static int directory_valid(int fd)
{
    struct stat st;
    struct statfs fs;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || fstatfs(fd, &fs) < 0 || fs.f_type != CGROUP2_SUPER_MAGIC) return 0;
    int top = open("/sys/fs/cgroup", O_PATH | O_DIRECTORY | O_CLOEXEC);
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n >= 0) path[n] = 0;
    const char *base = n >= 0 ? strrchr(path, '/') : NULL;
    int app = base && strncmp(base + 1, "app_", 4) == 0 && base[5] != 0;
    int valid = app && top >= 0 && !same_directory(top, fd) && beneath(fd, top) &&
        read_number(fd, "cgroup.freeze") == 0;
    if (top >= 0) close(top);
    return valid;
}

static int finish_commit(void *data)
{
    struct guard *g = data;
    if (frozen(g) != 1) {
        wl_event_source_timer_update(g->completion, 10);
        return 0;
    }
    if (send(g->client, "committed", 9, MSG_DONTWAIT | MSG_NOSIGNAL) != 9) {
        guard_drop(g, 1);
        return 0;
    }
    g->committed = 1;
    g->committing = 0;
    if (g->timer) { wl_event_source_remove(g->timer); g->timer = NULL; }
    if (g->completion) { wl_event_source_remove(g->completion); g->completion = NULL; }
    wl_event_source_remove(g->socket_source); g->socket_source = NULL;
    close(g->client); g->client = -1;
    return 0;
}

static int transaction(int fd, uint32_t mask, void *data)
{
    struct guard *g = data;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) && !(mask & WL_EVENT_READABLE)) {
        guard_drop(g, !g->committed); return 0;
    }
    char text[32] = { 0 }, control[CMSG_SPACE(4 * sizeof(int))] = { 0 };
    struct iovec iov = { text, sizeof text - 1 };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof control };
    ssize_t n = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n < 0) { guard_drop(g, !g->committed); return 0; }
    int directory = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int next; memcpy(&next, (char *)CMSG_DATA(h) + sizeof(int) * i, sizeof next);
            if (directory < 0) directory = next; else close(next);
        }
    }
    if (n == 0) {
        if (directory >= 0) close(directory);
        guard_drop(g, !g->committed);
        return 0;
    }
    const char *reply = "error invalid transaction";
    if (n > 0 && !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
        if (strcmp(text, "begin") == 0 && g->directory < 0 && directory >= 0 && directory_valid(directory)) {
            struct guard *other;
            int busy = 0;
            wl_list_for_each(other, &guards, link) {
                if (other != g && same_directory(other->directory, directory)) busy = 1;
            }
            if (!busy) {
                g->directory = directory; directory = -1;
                g->events = openat(g->directory, "cgroup.events", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (g->events >= 0) {
                    char path[64];
                    snprintf(path, sizeof path, "/proc/self/fd/%d/cgroup.freeze", g->directory);
                    g->watch = watches >= 0 ? inotify_add_watch(watches, path, IN_MODIFY | IN_DELETE_SELF) : -1;
                    if (g->watch >= 0) reply = "ready";
                }
            } else reply = "error freeze already in progress";
        } else if (strcmp(text, "commit") == 0 && g->directory >= 0 && !g->committing) {
            /* The owner of the live gate performs the freezer write. A late
             * helper cannot freeze after this transaction has timed out. */
            int freezer = openat(g->directory, "cgroup.freeze", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
            if (freezer >= 0) {
                ssize_t written = write(freezer, "1", 1);
                close(freezer);
                if (written == 1) {
                    g->committing = 1;
                    g->completion = wl_event_loop_add_timer(server.wl_event_loop, finish_commit, g);
                    if (g->completion) {
                        wl_event_source_timer_update(g->completion, 1);
                        if (directory >= 0) close(directory);
                        return 0;
                    }
                }
            }
        } else if (strcmp(text, "abort") == 0) reply = "aborted";
    }
    if (directory >= 0) close(directory);
    ssize_t sent = send(fd, reply, strlen(reply), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (strcmp(reply, "ready") == 0 && sent == 5) return 0;
    guard_drop(g, !g->committed);
    return 0;
}

static int accept_transaction(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    int client = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) return 0;
    struct ucred cred; socklen_t size = sizeof cred;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &size) < 0 || cred.uid != 0) { close(client); return 0; }
    struct guard *g = calloc(1, sizeof *g);
    if (!g) { close(client); return 0; }
    g->client = client; g->directory = g->events = g->watch = -1;
    wl_list_insert(&guards, &g->link);
    g->socket_source = wl_event_loop_add_fd(server.wl_event_loop, client, WL_EVENT_READABLE, transaction, g);
    g->timer = wl_event_loop_add_timer(server.wl_event_loop, deadline, g);
    if (!g->socket_source || !g->timer) { guard_drop(g, 1); return 0; }
    wl_event_source_timer_update(g->timer, 6000);
    return 0;
}

bool locachy_clipboard_allowed(bool primary)
{
    if (!sender || !request_interface || !request_name) return true;
    const char *interface = primary ? "zwp_primary_selection_device_v1" : "wl_data_device";
    if (strcmp(request_interface, interface) != 0 || strcmp(request_name, "set_selection") != 0) return true;
    struct tracked_client *c;
    wl_list_for_each(c, &tracked, link) {
        if (c->client != sender) continue;
        /* The connection credentials identify its creator. Pin that PID
         * with pidfd before consulting /proc again; inherited connections
         * retain the original cgroup if the creator has gone. */
        struct pollfd p = { .fd = c->pidfd, .events = POLLIN };
        if (c->pidfd >= 0 && poll(&p, 1, 0) == 0) {
            int current = process_cgroup(c->pid);
            p.revents = 0;
            if (current >= 0 && poll(&p, 1, 0) == 0) {
                if (c->cgroup >= 0) close(c->cgroup);
                c->cgroup = current;
            } else if (current >= 0) close(current);
        }
        if (c->cgroup < 0) return false;
        struct guard *g;
        wl_list_for_each(g, &guards, link) {
            if (g->directory >= 0 && beneath(c->cgroup, g->directory)) {
                if (!g->committed || read_number(g->directory, "cgroup.freeze") == 1) return false;
            }
        }
        /* A compositor restarted while the cgroup was frozen has no old
         * guard table: the kernel remains authoritative. */
        return read_number(c->cgroup, "cgroup.freeze") != 1;
    }
    return false;
}

void locachy_clipboard_init(void)
{
    wl_list_init(&tracked); wl_list_init(&guards);
    created.notify = client_created;
    wl_display_add_client_created_listener(server.wl_display, &created);
    logger = wl_display_add_protocol_logger(server.wl_display, protocol_request, NULL);
    watches = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watches >= 0) watch_source = wl_event_loop_add_fd(server.wl_event_loop, watches, WL_EVENT_READABLE, changed, NULL);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || snprintf(socket_path, sizeof socket_path, "%s/locachy-clipboard-guard.sock", runtime) >= (int)sizeof socket_path) return;
    listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) return;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, socket_path);
    unlink(socket_path);
    mode_t old = umask(0177);
    int n = bind(listener, (struct sockaddr *)&address, sizeof address);
    umask(old);
    if (n < 0 || listen(listener, 16) < 0 || !logger || watches < 0 || !watch_source) goto fail;
    listener_source = wl_event_loop_add_fd(server.wl_event_loop, listener, WL_EVENT_READABLE, accept_transaction, NULL);
    if (listener_source) return;
fail:
    wlr_log(WLR_ERROR, "LOCachy clipboard freeze guard unavailable");
    close(listener); listener = -1;
    unlink(socket_path);
}

void locachy_clipboard_finish(void)
{
    if (listener_source) wl_event_source_remove(listener_source);
    if (listener >= 0) close(listener);
    if (socket_path[0]) unlink(socket_path);
    struct guard *g, *gt;
    wl_list_for_each_safe(g, gt, &guards, link) guard_drop(g, !g->committed);
    struct tracked_client *c, *ct;
    wl_list_for_each_safe(c, ct, &tracked, link) client_destroy(&c->destroy, NULL);
    if (logger) wl_protocol_logger_destroy(logger);
    wl_list_remove(&created.link);
    if (watch_source) wl_event_source_remove(watch_source);
    if (watches >= 0) close(watches);
}
