/*
 * extdisplay's end of extdisplay-ipc.h: starting extdisplay-worker as nobody with no groups and no
 * new privileges, and checking everything it sends. A message of the wrong
 * size, a descriptor where none belongs, or a picture memfd that is not
 * sealed or not exactly its picture's size is the worker having failed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-ipc.h"
#include "extdisplay-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* In the child, before exec: every step works, or it exits. */
static void child(int ipc, int transport, int render_fd)
{
    int s = fcntl(ipc, F_DUPFD_CLOEXEC, 10);
    int t = fcntl(transport, F_DUPFD_CLOEXEC, 10);
    int r = render_fd >= 0 ? fcntl(render_fd, F_DUPFD_CLOEXEC, 10) : -1;
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    sigset_t none;
    if (s < 0 || t < 0 || (render_fd >= 0 && r < 0) || null < 0 ||
        dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0 ||
        dup2(s, EXTDISPLAY_IPC_FD) < 0 || dup2(t, EXTDISPLAY_TRANSPORT_FD) < 0 ||
        (r >= 0 && dup2(r, EXTDISPLAY_RENDER_FD) < 0) ||
        close_range((r >= 0 ? EXTDISPLAY_RENDER_FD : EXTDISPLAY_TRANSPORT_FD) + 1, ~0u, 0) < 0) {
        _exit(127);
    }
    sigemptyset(&none);
    if (setsid() < 0 || sigprocmask(SIG_SETMASK, &none, NULL) < 0 ||
        setgroups(0, NULL) < 0 || setgid(EXTDISPLAY_WORKER_USER) < 0 || setuid(EXTDISPLAY_WORKER_USER) < 0 ||
        getuid() != EXTDISPLAY_WORKER_USER || geteuid() != EXTDISPLAY_WORKER_USER || setuid(0) == 0 ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || chdir("/") < 0) {
        _exit(127);
    }
    char *const argv[] = { "extdisplay-worker", NULL };
    char *const env[] = { "LANG=C", "PATH=/usr/bin", NULL };
    execve(EXTDISPLAY_WORKER_PATH, argv, env);
    _exit(127);
}

pid_t extdisplay_worker_spawn_hw(int transport, int render_fd, int *ipc)
{
    int sv[2];
    if (geteuid() != 0) {
        errno = EPERM; /* it could not drop to nobody */
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) child(sv[1], transport, render_fd);
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        return -1;
    }
    *ipc = sv[0];
    return pid;
}

pid_t extdisplay_worker_spawn(int transport, int *ipc)
{
    return extdisplay_worker_spawn_hw(transport, -1, ipc);
}

int extdisplay_ipc_send(int fd, struct extdisplay_ipc *m)
{
    m->magic = EXTDISPLAY_IPC_MAGIC;
    m->reserved = 0;
    ssize_t n;
    do n = send(fd, m, sizeof *m, MSG_DONTWAIT | MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof *m ? 0 : -1;
}

int extdisplay_ipc_recv_fds(int fd, struct extdisplay_ipc *m, int *fds, unsigned max_fds, unsigned *nfds_out)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(8 * sizeof(int))]; } c;
    struct iovec iov = { m, sizeof *m };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = c.bytes, .msg_controllen = sizeof c.bytes };
    ssize_t n;
    for (unsigned i = 0; i < max_fds; i++) fds[i] = -1;
    if (nfds_out) *nfds_out = 0;
    do n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    unsigned nfds = 0;
    int bad = 0;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || h->cmsg_len < CMSG_LEN(0)) {
            bad = 1;
            continue;
        }
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int f;
            memcpy(&f, (char *)CMSG_DATA(h) + i * sizeof f, sizeof f);
            if (nfds < max_fds) fds[nfds] = f;
            else close(f);
            nfds++;
        }
    }
    unsigned want_min = 0, want_max = 0;
    if (m->op == EXTDISPLAY_IPC_FRAME) {
        want_min = want_max = 1;
    } else if (m->op == EXTDISPLAY_IPC_DMABUF) {
        want_min = 1;
        want_max = EXTDISPLAY_DMABUF_MAX_PLANES;
    }
    int dmabuf_bad = 0;
    if (m->op == EXTDISPLAY_IPC_DMABUF) {
        if (m->length != sizeof(struct extdisplay_dmabuf)) {
            dmabuf_bad = 1;
        } else {
            struct extdisplay_dmabuf info;
            memcpy(&info, m->text, sizeof info);
            if (info.num_planes < 1 || info.num_planes > EXTDISPLAY_DMABUF_MAX_PLANES ||
                info.num_objects < 1 || info.num_objects > EXTDISPLAY_DMABUF_MAX_PLANES ||
                info.num_objects != nfds) {
                dmabuf_bad = 1;
            }
            for (uint32_t i = 0; !dmabuf_bad && i < info.num_planes; i++) {
                if (info.object_index[i] >= info.num_objects) dmabuf_bad = 1;
            }
        }
    }
    if (n != (ssize_t)sizeof *m || bad || nfds < want_min || nfds > want_max || nfds > max_fds ||
        (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        m->magic != EXTDISPLAY_IPC_MAGIC || m->reserved || m->length > EXTDISPLAY_IPC_TEXT ||
        (m->op != EXTDISPLAY_IPC_FRAME && m->bytes) || dmabuf_bad) {
        for (unsigned i = 0; i < max_fds && i < nfds; i++) {
            if (fds[i] >= 0) close(fds[i]);
            fds[i] = -1;
        }
        errno = EPROTO;
        return -1;
    }
    if (nfds_out) *nfds_out = nfds;
    return 0;
}

int extdisplay_ipc_recv(int fd, struct extdisplay_ipc *m, int *pixels)
{
    int fds[1] = { -1 };
    unsigned nfds = 0;
    int r = extdisplay_ipc_recv_fds(fd, m, fds, 1, &nfds);
    if (pixels) *pixels = r == 0 && nfds >= 1 ? fds[0] : -1;
    else if (r == 0 && nfds >= 1 && fds[0] >= 0) close(fds[0]);
    return r;
}

void *extdisplay_frame_map(int fd, const struct extdisplay_ipc *m)
{
    const int need = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL;
    size_t expected;
    struct stat st;
    int seals = fd < 0 ? -1 : fcntl(fd, F_GET_SEALS);
    if (m->op != EXTDISPLAY_IPC_FRAME || m->length || extdisplay_dimensions(m->width, m->height, &expected) < 0 ||
        m->bytes != expected || seals < 0 || (seals & need) != need ||
        fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)expected) {
        return NULL;
    }
    void *p = mmap(NULL, expected, PROT_READ, MAP_PRIVATE, fd, 0);
    return p == MAP_FAILED ? NULL : p;
}
