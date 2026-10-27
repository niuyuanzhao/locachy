/* The control channel between process 1 and its programs (see internal.h),
 * and the calls programs make through it (base.h). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

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
    if (strcmp(msg, "take") == 0 && n >= 1) {
        screen_take(fds[0]);
        keys_reload(); /* rc.conf's keys and the key table, as settings may have left them */
        keys_take(fds + 1, n - 1);
        pointer_resume();
        shown = 1;
        return 1;
    }
    if (strcmp(msg, "keys") == 0) {
        keys_update(fds, n);
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
    static const char *const want[] = { "back", "ended", NULL };
    char msg[CONTROL_MAX], *f[3];
    int n = ask("open", argv, want, msg, sizeof msg, f, 3);

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

int base_app_open(const char *name, const char *target, char *why, size_t size)
{
    static const char *const want[] = { "app-result", NULL };
    char msg[CONTROL_MAX], *fields[4];
    char *argv[] = { "open", (char *)name, (char *)target, NULL };
    int n = ask("app", argv, want, msg, sizeof msg, fields, 4);

    if (size > 0) {
        snprintf(why, size, "%s", n == 4 ? fields[3] : "container control unavailable");
    }
    return n == 4 && strcmp(fields[1], "ok") == 0 ? 0 : -1;
}

int open_in_home(const char *with, const char *target)
{
    static const char *const home[] = { "home", NULL };
    char tmp[64];
    if (strchr(target, '\n') || (with && strchr(with, '\n'))) {
        return -1;
    }
    /* Whole or not at all: home may read it as soon as it is there. */
    snprintf(tmp, sizeof tmp, "%s.%d", OPEN_REQUEST, (int)getpid());
    FILE *out = fopen(tmp, "we");
    if (!out) {
        return -1;
    }
    int ok = fprintf(out, "%s\n%s\n", with ? with : "", target) > 0;
    if (fclose(out) != 0 || !ok || rename(tmp, OPEN_REQUEST) != 0) {
        unlink(tmp);
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
