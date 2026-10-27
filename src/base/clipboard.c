/* Local access to the host clipboard. No compositor or application is
 * contacted by the caller: even with the display frozen, text is in RAM. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "clipboard.h"

static int64_t milliseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int64_t until)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int64_t left = until - milliseconds();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int n = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (n > 0) {
            return 0;
        }
        if (n == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

int clipboard_request(struct clipboard_message *message, int *fd, int timeout_ms)
{
    const char *path = getenv("LOCACHY_CLIPBOARD_SOCKET");
    if (!path) path = CLIPBOARD_SOCKET;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (path[0] != '/' || strlen(path) >= sizeof address.sun_path) {
        errno = EINVAL;
        return CLIPBOARD_FAILED;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0) return CLIPBOARD_FAILED;
    int result = CLIPBOARD_FAILED;
    int64_t until = milliseconds() + timeout_ms;
    if (connect(sock, (struct sockaddr *)&address, sizeof address) < 0) {
        if (errno != EINPROGRESS || wait_fd(sock, POLLOUT, until) < 0) goto done;
        int error = 0;
        socklen_t len = sizeof error;
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            if (error) errno = error;
            goto done;
        }
    }
    char space[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = message, .iov_len = sizeof *message };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (*fd >= 0) {
        m.msg_control = space;
        m.msg_controllen = sizeof space;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), fd, sizeof *fd);
    }
    for (;;) {
        if (sendmsg(sock, &m, MSG_NOSIGNAL) == sizeof *message) break;
        if (errno != EAGAIN && errno != EINTR) goto done;
        if (wait_fd(sock, POLLOUT, until) < 0) goto done;
    }
    for (;;) {
        if (wait_fd(sock, POLLIN, until) < 0) goto done;
        memset(space, 0, sizeof space);
        m.msg_control = space;
        m.msg_controllen = sizeof space;
        m.msg_flags = 0;
        ssize_t n = recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        int received = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int other;
                memcpy(&other, (char *)CMSG_DATA(c) + i * sizeof other, sizeof other);
                if (received < 0) received = other; else close(other);
            }
        }
        if (n != sizeof *message || message->version != CLIPBOARD_VERSION ||
            (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
            if (received >= 0) close(received);
            errno = EPROTO;
            goto done;
        }
        if (message->status > CLIPBOARD_FAILED) {
            if (received >= 0) close(received);
            *fd = -1;
            errno = EPROTO;
            goto done;
        }
        *fd = received;
        result = (int)message->status;
        break;
    }
done:
    close(sock);
    return result;
}

static int get_as(uint32_t operation, int primary, int *fd, size_t *size)
{
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = operation,
        .selection = primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR };
    *fd = -1;
    int status = clipboard_request(&m, fd, CLIPBOARD_TIMEOUT_MS);
    if (status == CLIPBOARD_OK && (*fd < 0 || m.length > SIZE_MAX)) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        return CLIPBOARD_FAILED;
    }
    *size = status == CLIPBOARD_OK ? (size_t)m.length : 0;
    return status;
}

int clipboard_get(int primary, int *fd, size_t *size)
{
    return get_as(CLIPBOARD_GET, primary, fd, size);
}

int clipboard_get_png(int *fd, size_t *size)
{
    return get_as(CLIPBOARD_GET_PNG, 0, fd, size);
}

static int set_as(uint32_t operation, int primary, const void *text, size_t size)
{
    int data = memfd_create("clipboard-data", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (data < 0) return CLIPBOARD_FAILED;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(data, (const char *)text + written, size - written);
        if (n > 0) written += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(data); return CLIPBOARD_FAILED; }
    }
    if (fcntl(data, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(data);
        return CLIPBOARD_FAILED;
    }
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = operation,
        .selection = primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR, .length = size };
    int fd = data;
    int result = clipboard_request(&m, &fd, CLIPBOARD_TIMEOUT_MS);
    if (fd >= 0 && fd != data) close(fd);
    close(data);
    return result;
}

int clipboard_set(int primary, const void *text, size_t size)
{
    return set_as(CLIPBOARD_SET, primary, text, size);
}

int clipboard_set_png(const void *png, size_t size)
{
    return set_as(CLIPBOARD_SET_PNG, 0, png, size);
}

int clipboard_clear(int primary)
{
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = CLIPBOARD_CLEAR,
        .selection = primary ? CLIPBOARD_PRIMARY : CLIPBOARD_REGULAR };
    int fd = -1;
    int result = clipboard_request(&m, &fd, CLIPBOARD_TIMEOUT_MS);
    if (fd >= 0) close(fd);
    return result;
}

int clipboard_sync(uint64_t *generation)
{
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = CLIPBOARD_SYNC };
    int fd = -1;
    int result = clipboard_request(&m, &fd, CLIPBOARD_TIMEOUT_MS);
    if (fd >= 0) close(fd);
    *generation = m.generation;
    return result;
}

int clipboard_check(uint64_t generation)
{
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = CLIPBOARD_CHECK,
        .generation = generation };
    int fd = -1;
    int result = clipboard_request(&m, &fd, CLIPBOARD_TIMEOUT_MS);
    if (fd >= 0) close(fd);
    return result;
}

static int guard_message(int socket, const char *text, int directory, const char *expected, int64_t until)
{
    struct iovec iov = { .iov_base = (void *)text, .iov_len = strlen(text) };
    char space[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (directory >= 0) {
        message.msg_control = space; message.msg_controllen = sizeof space;
        struct cmsghdr *h = CMSG_FIRSTHDR(&message);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &directory, sizeof directory);
    }
    for (;;) {
        if (milliseconds() >= until) { errno = ETIMEDOUT; return -1; }
        if (sendmsg(socket, &message, MSG_NOSIGNAL) == (ssize_t)strlen(text)) break;
        if (errno != EINTR && errno != EAGAIN) return -1;
        if (wait_fd(socket, POLLOUT, until) < 0) return -1;
    }
    char reply[128];
    for (;;) {
        if (wait_fd(socket, POLLIN, until) < 0) return -1;
        ssize_t n = recv(socket, reply, sizeof reply - 1, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return -1;
        reply[n] = 0;
        return strcmp(reply, expected) == 0 ? 0 : -1;
    }
}

int clipboard_freeze(const char *cgroup)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *path = getenv("LOCACHY_CLIPBOARD_GUARD_SOCKET");
    char address_path[108];
    if (!path) {
        if (!runtime || snprintf(address_path, sizeof address_path, "%s/locachy-clipboard-guard.sock", runtime) >= (int)sizeof address_path) return CLIPBOARD_FAILED;
        path = address_path;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (path[0] != '/' || strlen(path) >= sizeof address.sun_path) return CLIPBOARD_FAILED;
    strcpy(address.sun_path, path);
    int dir = open(cgroup, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return CLIPBOARD_FAILED;
    int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int result = CLIPBOARD_FAILED;
    int64_t until = milliseconds() + 5500;
    if (socket_fd < 0 || connect(socket_fd, (struct sockaddr *)&address, sizeof address) < 0) goto done;
    if (guard_message(socket_fd, "begin", dir, "ready", until) < 0) goto done;
    int64_t left = until - milliseconds();
    if (left <= 0) goto done;
    struct clipboard_message m = { .version = CLIPBOARD_VERSION, .operation = CLIPBOARD_SYNC };
    int received = -1;
    result = clipboard_request(&m, &received, left < CLIPBOARD_TIMEOUT_MS ? (int)left : CLIPBOARD_TIMEOUT_MS);
    if (received >= 0) close(received);
    if (result != CLIPBOARD_OK) goto done;
    result = CLIPBOARD_FAILED;
    if (milliseconds() >= until) goto done;
    /* The compositor writes freeze only while the gate transaction exists;
     * an expired/disconnected transaction cannot perform the side effect. */
    if (guard_message(socket_fd, "commit", -1, "committed", until) == 0) result = CLIPBOARD_OK;
done:
    if (socket_fd >= 0) close(socket_fd);
    close(dir);
    return result;
}

#ifndef N_
#define N_(s) (s)
#endif

const char *clipboard_error(int status)
{
    switch (status) {
    case CLIPBOARD_OK: return "ok";
    case CLIPBOARD_EMPTY: return N_("clipboard is empty");
    case CLIPBOARD_NO_TEXT: return N_("clipboard has no text");
    case CLIPBOARD_BUSY: return N_("clipboard transfer is not complete");
    default: return N_("clipboard service unavailable or transfer failed");
    }
}
