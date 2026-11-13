/* The control channel between process 1 and its programs (see internal.h),
 * and the calls programs make through it (base.h). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "session.h"

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif
/* A separate restricted owner channel: never the display's privileged
 * control descriptor, and never given to a network worker. */
static int session = -2, session_allowed, session_cancelled, session_blanked;
static unsigned long long session_epoch, session_request;

int base_session_fd(void)
{
    if (session == -2) {
        const char *s = getenv("LOCACHY_SESSION_FD");
        char *end = NULL;
        long fd = s ? strtol(s, &end, 10) : -1;
        session = s && end && !*end && fd >= 3 && fd <= 1048576 ? (int)fd : -1;
        unsetenv("LOCACHY_SESSION_FD");
        if (session >= 0) fcntl(session, F_SETFD, FD_CLOEXEC);
    }
    return session;
}

static int session_policy(const char *text)
{
    unsigned long long epoch;
    int allowed, end = 0;
    if (sscanf(text, "policy %llu %d%n", &epoch, &allowed, &end) != 2 || text[end] ||
        (allowed != 0 && allowed != 1) || epoch < session_epoch) return 0;
    int changed = epoch != session_epoch || allowed != session_allowed;
    session_epoch = epoch;
    session_allowed = allowed;
    if (changed) session_cancelled = 0;
    return changed;
}

int base_session_service(void)
{
    int fd = base_session_fd(), changed = 0;
    if (fd < 0) return -1;
    char text[160];
    for (;;) {
        ssize_t n = recv(fd, text, sizeof text - 1, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return changed;
        if (n <= 0) {
            close(fd);
            session = -1;
            session_allowed = session_cancelled = 0;
            return -1;
        }
        text[n] = 0;
        changed |= session_policy(text);
    }
}

int base_session_input_allowed(void)
{
    return base_session_fd() >= 0 && session_allowed && session_cancelled;
}

/* Bounded requests: unsolicited revocations are processed before replies.
 * A request never retries after a possible side effect. */
static int session_ask(const char *verb, int value)
{
    int fd = base_session_fd();
    if (fd < 0) return -1;
    base_session_service();
    if (session < 0) return -1;
    unsigned long long id = ++session_request, epoch = session_epoch;
    char text[160];
    int len = snprintf(text, sizeof text, "%s %llu %llu %d", verb, id, epoch, value);
    if (send(fd, text, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT) != len) return -1;
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &current);
        long left = 1000 - (current.tv_sec - start.tv_sec) * 1000 -
                    (current.tv_nsec - start.tv_nsec) / 1000000;
        if (left <= 0) return -1;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, (int)left) < 0 && errno == EINTR) continue;
        if (!(p.revents & POLLIN)) return -1;
        ssize_t n = recv(fd, text, sizeof text - 1, MSG_DONTWAIT);
        if (n <= 0) return -1;
        text[n] = 0;
        if (strncmp(text, "policy ", 7) == 0) { session_policy(text); continue; }
        unsigned long long got, got_epoch;
        int ok, end = 0;
        if (sscanf(text, "result %llu %llu %d%n", &got, &got_epoch, &ok, &end) == 3 &&
            !text[end] && got == id) return ok && got_epoch == epoch && session_epoch == epoch ? 0 : -1;
    }
}

int base_session_input_cancelled(void)
{
    if (session_blanked && (!base_screen_power || base_screen_power(1) != 0)) return -1;
    session_blanked = 0;
    if (session_ask("cancelled", 0) != 0) return -1;
    session_cancelled = 1;
    return 0;
}

int base_sleep_inhibit(int enabled)
{
    if (enabled && !base_session_input_allowed()) return -1;
    return session_ask("inhibit", !!enabled);
}

int base_display_power(int on)
{
    if (!on && !base_session_input_allowed()) return -1;
    if (session_ask("display", !!on) != 0 || !base_screen_power) return -1;
    if (base_screen_power(!!on) != 0) return -1;
    session_blanked = !on;
    return 0;
}

int base_session_hold(int on)
{
    int fd = base_session_fd();
    if (fd < 0) return -1;
    char text[16];
    int len = snprintf(text, sizeof text, "hold %d", on ? 1 : 0);
    return send(fd, text, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT) == len ? 0 : -1;
}

static int ctl = -2; /* -2: $LOCACHY_CONTROL not read yet */
static int shown;    /* a "take" arrived that key_get has not reported */

int control_fd(void)
{
    if (ctl == -2) {
        const char *s = getenv("LOCACHY_CONTROL");
        ctl = s != NULL ? atoi(s) : -1;
        /* Keep it from children this program may start (a shell in term, the
         * shared display's compositor and its clients): the descriptor, and
         * the name, as a child would find another descriptor at that number
         * (its own Wayland connection) and break it with a control message. */
        unsetenv("LOCACHY_CONTROL");
        if (ctl >= 0) {
            fcntl(ctl, F_SETFD, FD_CLOEXEC);
        }
    }
    return ctl;
}

int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds)
{
    char space[CMSG_SPACE(sizeof(int) * CONTROL_MAX_FDS)] = { 0 };
    struct iovec iov = { .iov_base = (void *)msg, .iov_len = len };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };

    if (nfds > 0) {
        m.msg_control = space;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    return sendmsg(sock, &m, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

int control_send_fields(int sock, const char *const *fields)
{
    char msg[CONTROL_MAX];
    size_t len = 0;

    for (; *fields != NULL; fields++) {
        size_t n = strlen(*fields) + 1;
        if (len + n > sizeof msg) {
            return -1;
        }
        memcpy(msg + len, *fields, n);
        len += n;
    }
    return control_send(sock, msg, len ? len - 1 : 0, NULL, 0);
}

int control_recv(int sock, char *msg, size_t size, int *fds, int *nfds)
{
    char space[CMSG_SPACE(sizeof(int) * CONTROL_MAX_FDS)];
    struct iovec iov = { .iov_base = msg, .iov_len = size - 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = space, .msg_controllen = sizeof space };

    *nfds = 0;
    ssize_t n = recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return -1;
    }
    msg[n] = '\0';
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c != NULL; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + *nfds, CMSG_DATA(c), sizeof(int) * (size_t)k);
            *nfds += k;
        }
    }
    return (int)n;
}

int control_fields(char *msg, int len, char **fields, int max)
{
    int n = 0;

    for (int i = 0; i < len && n < max; i += (int)strlen(msg + i) + 1) {
        fields[n++] = msg + i;
    }
    return n;
}

int control_service(char *other, size_t size, int *len)
{
    char msg[CONTROL_MAX];
    int fds[CONTROL_MAX_FDS], n;

    int got = control_fd() < 0 ? 0 : control_recv(ctl, msg, sizeof msg, fds, &n);
    if (got <= 0) {
        return 0;
    }
    /* "take" and "keys" may carry the locks, process 1's, as a second field. */
    int locks = got > 5 && (strcmp(msg, "take") == 0 || strcmp(msg, "keys") == 0) ? atoi(msg + 5) : -1;
    if (strcmp(msg, "take") == 0 && n >= 1) {
        screen_take(fds[0]);
        keys_reload(); /* rc.conf's keys and the key table, as settings may have left them */
        keys_take(fds + 1, n - 1);
        if (locks >= 0) keys_set_locks(locks);
        pointer_resume();
        shown = 1;
        return 1;
    }
    if (strcmp(msg, "keys") == 0) {
        int held[CONTROL_MAX_FDS], had = keys_fds(held, CONTROL_MAX_FDS);
        keys_update(fds, n);
        if (locks >= 0 && had == 0 && n > 0) keys_set_locks(locks); /* a program's own presses stand */
        return 1;
    }
    if (strcmp(msg, "pointer-retry") == 0 && n == 0) {
        pointer_retry();
        return 1;
    }
    if (strncmp(msg, "pointer-remove", 15) == 0 && n == 0 && got > 15) {
        pointer_remove(msg + 15);
        return 1;
    }
    if (strncmp(msg, "pointer", 8) == 0 && n == 1 && got > 8) {
        pointer_take(fds[0], msg + 8); /* "pointer" NUL PATH */
        return 1;
    }
    if (n == 0 && (strcmp(msg, "screens") == 0 || strcmp(msg, "pointer-enter") == 0 ||
                   strcmp(msg, "pointer-left") == 0)) {
        char *fields[1 + 5 * 8];
        int count = control_fields(msg, got, fields, (int)(sizeof fields / sizeof fields[0]));
        if (strcmp(msg, "screens") == 0 && screen_desk) {
            screen_desk(fields + 1, count - 1); /* before a take: where its screens are */
        } else if (strcmp(msg, "pointer-enter") == 0 && count == 3) {
            pointer_came(atoi(fields[1]), atoi(fields[2]));
        } else if (strcmp(msg, "pointer-left") == 0) {
            pointer_went();
        }
        return 1;
    }
    for (int i = 0; i < n; i++) {
        close(fds[i]);
    }
    if (strcmp(msg, "release") == 0) {
        screen_release();
        keys_release();
        pointer_release();
        control_send(ctl, "released", 8, NULL, 0);
        return 1;
    }
    if (other != NULL && (size_t)got < size) {
        memcpy(other, msg, (size_t)got + 1);
        *len = got;
        return 2;
    }
    return 1;
}

void control_wait_take(void)
{
    while (!shown && control_service(NULL, 0, NULL)) {
    }
}

int control_shown(void)
{
    int s = shown;

    shown = 0;
    return s;
}

/* Send verb ARG... and wait for an answer whose first field is one of
 * want (up to a NULL); its fields go into fields. Returns how many fields,
 * 0 if there is no process 1 to ask. */
static int ask(const char *verb, char *const argv[], const char *const *want,
               char *msg, size_t size, char **fields, int max)
{
    const char *out[16] = { verb };
    int n = 1, len;

    for (; argv != NULL && argv[n - 1] != NULL && n < 15; n++) {
        out[n] = argv[n - 1];
    }
    out[n] = NULL;
    if (control_fd() < 0 || control_send_fields(ctl, out) < 0) {
        return 0;
    }
    for (;;) {
        int r = control_service(msg, size, &len);
        if (r == 0) {
            return 0;
        }
        if (r != 2) {
            continue;
        }
        for (const char *const *w = want; *w != NULL; w++) {
            if (strcmp(msg, *w) == 0) {
                return control_fields(msg, len, fields, max);
            }
        }
    }
}

int base_call(char *const argv[])
{
    static const char *const want[] = { "returned", NULL };
    char msg[CONTROL_MAX], *f[3];

    return ask("call", argv, want, msg, sizeof msg, f, 3) == 3 ? atoi(f[2]) : -1;
}

void base_open(char *const argv[], char *why, size_t size)
{
    base_open_on(NULL, argv, why, size);
}

void base_open_on(const char *screen, char *const argv[], char *why, size_t size)
{
    static const char *const want[] = { "back", "ended", "shown", NULL };
    char msg[CONTROL_MAX], *f[3], *on[16];
    int k = 0;
    if (screen) on[k++] = (char *)screen;
    for (int i = 0; argv[i] && k < 15; i++) on[k++] = argv[i];
    on[k] = NULL;
    int n = ask(screen ? "open-on" : "open", on, want, msg, sizeof msg, f, 3);

    if (n == 0) {
        snprintf(why, size, "no process 1 to ask");
    } else if (n == 3) {
        snprintf(why, size, "%s %s %s", f[0], f[1], f[2]);
    } else {
        snprintf(why, size, "%s %s", f[0], n > 1 ? f[1] : "?");
    }
}

void base_tasks(char *buf, size_t size)
{
    static const char *const want[] = { "tasks", NULL };
    char msg[CONTROL_MAX], *f[32];
    int n = ask("tasks?", NULL, want, msg, sizeof msg, f, 32);
    size_t len = 0;

    buf[0] = '\0';
    for (int i = 1; i < n && len < size; i++) {
        len += (size_t)snprintf(buf + len, size - len, "%s\n", f[i]);
    }
}

void base_kill(char *const argv[])
{
    static const char *const want[] = { "ended", NULL };
    char msg[CONTROL_MAX], *f[3];

    ask("kill", argv, want, msg, sizeof msg, f, 3);
}

void base_apps(char *buf, size_t size)
{
    static const char *const want[] = { "apps", NULL };
    char msg[CONTROL_MAX], *fields[2];
    int n = ask("apps?", NULL, want, msg, sizeof msg, fields, 2);

    if (size > 0) {
        snprintf(buf, size, "%s", n == 2 ? fields[1] : "");
    }
}

int base_app_action(const char *action, const char *name, char *why, size_t size)
{
    static const char *const want[] = { "app-result", NULL };
    char msg[CONTROL_MAX], *fields[4];
    char *argv[] = { (char *)action, (char *)name, NULL };
    int n = ask("app", argv, want, msg, sizeof msg, fields, 4);

    if (size > 0) {
        snprintf(why, size, "%s", n == 4 ? fields[3] : "container control unavailable");
    }
    return n == 4 && strcmp(fields[1], "ok") == 0 ? 0 : -1;
}

static int app_open_verb(const char *verb, const char *name, const char *target, char *why, size_t size)
{
    static const char *const want[] = { "app-result", NULL };
    char msg[CONTROL_MAX], *fields[4];
    char *argv[] = { (char *)verb, (char *)name, (char *)target, NULL };
    int n = ask("app", argv, want, msg, sizeof msg, fields, 4);

    if (size > 0) {
        snprintf(why, size, "%s", n == 4 ? fields[3] : "container control unavailable");
    }
    return n == 4 && strcmp(fields[1], "ok") == 0 ? 0 : -1;
}

int base_app_open(const char *name, const char *target, char *why, size_t size)
{
    return app_open_verb("open", name, target, why, size);
}

int base_app_open_own_tor(const char *name, const char *target, char *why, size_t size)
{
    return app_open_verb("open-own-tor", name, target, why, size);
}

/* Published by open.c for programs. Process 1 does not link open.c (make.sh),
 * so this is weak: process 1 never calls open_in_home. */
__attribute__((weak)) int open_request_publish(const char *with, const char *target);

int open_in_home(const char *with, const char *target)
{
    static const char *const home[] = { "home", NULL };
    if (strchr(target, '\n') || (with && strchr(with, '\n'))) {
        return -1;
    }
    /* One unique request in the queue (open.c), never a shared slot that a
     * second producer could overwrite. */
    if (!open_request_publish || open_request_publish(with, target) != 0) {
        return -1;
    }
    if (control_fd() >= 0) {
        control_send_fields(ctl, home);
    }
    return 0;
}

void base_reboot(void)
{
    static const char *const msg[] = { "reboot", NULL };

    if (control_fd() >= 0) {
        control_send_fields(ctl, msg);
    }
    pause();
}

void base_poweroff(void)
{
    static const char *const msg[] = { "poweroff", NULL };

    if (control_fd() >= 0) {
        control_send_fields(ctl, msg);
    }
    pause();
}
