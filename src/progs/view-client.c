/*
 * view's end of view-proto.h: starting view-render as the user in its
 * sandbox, and the messages to and from it. Nothing view-render sends is
 * trusted: a reply of the wrong size, with more than one descriptor or a
 * memfd that can still shrink is treated as view-render having gone.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "view-proto.h"

/* Tests run a renderer they built; never chosen at run time (view is root). */
#ifdef VIEW_RENDER_TEST_PATH
#define RENDER_PATH VIEW_RENDER_TEST_PATH
#else
#define RENDER_PATH VIEW_RENDER_PATH
#endif

static uint32_t serial;

/* In the child, before exec: every step must work, or it exits. Only
 * async-signal-safe calls: the user's name and environment were looked up
 * before the fork. No supplementary group: the user's (input, video, wheel)
 * would give a broken renderer the keyboard or more. */
static void child(int sock, uid_t uid, gid_t gid, const char *home, char *const env[])
{
    int err = fcntl(2, F_DUPFD, 10);
    int s = fcntl(sock, F_DUPFD, 10);
    int null = open("/dev/null", O_RDWR);
    sigset_t none;

    if (s < 0 || null < 0) _exit(127);
    /* Its stderr is view's, for its log; a terminal there is not its own
     * controlling one after setsid, so it cannot push input into it. */
    if (dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(err >= 0 ? err : null, 2) < 0 ||
        dup2(s, VIEW_RENDER_FD) < 0 || close_range(VIEW_RENDER_FD + 1, ~0u, 0) < 0) {
        _exit(127);
    }
    sigemptyset(&none);
    if (setsid() < 0 || sigprocmask(SIG_SETMASK, &none, NULL) < 0 ||
        unshare(CLONE_NEWNET) < 0 ||
        setgroups(0, NULL) < 0 || setgid(gid) < 0 || setuid(uid) < 0 ||
        getuid() != uid || geteuid() != uid || setuid(0) == 0 ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || chdir(home) < 0) {
        _exit(127);
    }
    char *const argv[] = { "view-render", NULL };
    execve(RENDER_PATH, argv, env);
    _exit(127);
}

pid_t view_spawn(int *sock)
{
    struct passwd *pw = getpwuid(VIEW_USER);
    if (!pw || !pw->pw_dir || !pw->pw_name) return -1;
    static char env_home[4096 + 8], env_user[256 + 8];
    if (snprintf(env_home, sizeof env_home, "HOME=%s", pw->pw_dir) >= (int)sizeof env_home ||
        snprintf(env_user, sizeof env_user, "USER=%s", pw->pw_name) >= (int)sizeof env_user) {
        return -1;
    }
    char *const env[] = { env_home, env_user, "LANG=C.UTF-8", "PATH=/usr/bin", NULL };
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    const char *home = env_home + 5;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) child(sv[1], uid, gid, home, env);
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        return -1;
    }
    *sock = sv[0];
    return pid;
}

int view_send(int sock, struct view_request *r)
{
    r->serial = ++serial;
    r->text[VIEW_TEXT - 1] = '\0';
    ssize_t n;
    do n = send(sock, r, sizeof *r, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof *r ? 0 : -1;
}

int view_recv(int sock, struct view_reply *r, int *fd)
{
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } control;
    struct iovec iov = { r, sizeof *r };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = control.buf, .msg_controllen = sizeof control.buf };
    ssize_t n;

    *fd = -1;
    do n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    for (struct cmsghdr *c = n >= 0 ? CMSG_FIRSTHDR(&msg) : NULL; c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
            c->cmsg_len == CMSG_LEN(sizeof(int)) && *fd < 0) {
            memcpy(fd, CMSG_DATA(c), sizeof(int));
        } else if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            /* More descriptors than one: close them all, it is garbled. */
            int *fds = (int *)CMSG_DATA(c);
            for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) close(fds[i]);
            msg.msg_flags |= MSG_CTRUNC;
        }
    }
    if (n != (ssize_t)sizeof *r || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        (r->size > 0) != (*fd >= 0)) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        return -1;
    }
    r->text[VIEW_TEXT - 1] = '\0';
    return 0;
}

int view_call(int sock, struct view_request *r, struct view_reply *rep, int *fd)
{
    if (view_send(sock, r) < 0) return -1;
    for (;;) {
        if (view_recv(sock, rep, fd) < 0) return -1;
        if (rep->serial == r->serial) return rep->op == r->op ? 0 : -1;
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
}

void *view_map(int fd, uint64_t size)
{
    const int seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int have = fd < 0 ? -1 : fcntl(fd, F_GET_SEALS);
    struct stat st;

    /* Sealed, so view-render cannot shrink it under the mapping (SIGBUS). */
    if (have < 0 || (have & seals) != seals || size == 0 || size > SIZE_MAX ||
        fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size < size) {
        return NULL;
    }
    void *p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    return p == MAP_FAILED ? NULL : p;
}
