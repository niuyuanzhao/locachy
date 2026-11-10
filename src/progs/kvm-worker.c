/*
 * kvm-worker: the extended-display receiver's untrusted half (kvm-ipc.h).
 * It alone reads the paired stream: framing, JSON and video (distribution
 * FFmpeg, kvm-decode.c). It runs as nobody with no groups, no new
 * privileges, a Landlock ruleset that grants no path, resource limits and a
 * seccomp filter against new sockets, processes and signals; it refuses to
 * run without them. Pictures go to kvm as sealed memfds, one in flight; a
 * newer picture replaces an unsent one, never queues behind it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvm-ipc.h"
#include "kvm-decode.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TX_MAX (64u * 1024u)
#define POINTER_SHED (16u * 1024u) /* pending bytes above which pointer motion is dropped */
#define TIMEOUT_S 8.0      /* nothing from the peer: it is gone */
#define PING_S 2.0         /* OpenDisplay's cadence */
#define FLUSH_MS 100       /* a cancel is written within this, or the stream closes */

static struct {
    struct kvm_decoder *decoder;
    struct kvm_parser parser;
    unsigned char tx[TX_MAX];
    size_t tx_start, tx_end;
    double last_rx, last_ping;
    int hello, welcomed, active, frame_pending;
    const uint32_t *latest; /* the decoder's newest picture, not yet sent */
    uint32_t latest_w, latest_h;
} session;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int status(uint32_t op)
{
    struct kvm_ipc m = { .magic = KVM_IPC_MAGIC, .op = op };
    if (op == KVM_IPC_READY) {
        m.width = (kvm_decoder_available(KVM_CODEC_H264) ? 1u : 0u) | (kvm_decoder_available(KVM_CODEC_HEVC) ? 2u : 0u);
    }
    return send(KVM_IPC_FD, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof m ? 0 : -1;
}

static int enqueue(const void *data, size_t n)
{
    if (!n || n > KVM_CONTROL_MAX) return -1;
    if (session.tx_start) {
        memmove(session.tx, session.tx + session.tx_start, session.tx_end - session.tx_start);
        session.tx_end -= session.tx_start;
        session.tx_start = 0;
    }
    if (n + 4 > TX_MAX - session.tx_end) return -1; /* a peer that reads nothing */
    unsigned char *d = session.tx + session.tx_end;
    d[0] = (unsigned char)(n >> 24);
    d[1] = (unsigned char)(n >> 16);
    d[2] = (unsigned char)(n >> 8);
    d[3] = (unsigned char)n;
    memcpy(d + 4, data, n);
    session.tx_end += n + 4;
    return 0;
}

static int flush(void)
{
    while (session.tx_start < session.tx_end) {
        ssize_t n = send(KVM_TRANSPORT_FD, session.tx + session.tx_start, session.tx_end - session.tx_start,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;
        session.tx_start += (size_t)n;
    }
    session.tx_start = session.tx_end = 0;
    return 0;
}

/* Everything queued written, within FLUSH_MS: 0, or -1. */
static int flush_all(void)
{
    double deadline = now() + FLUSH_MS / 1000.0;
    while (session.tx_start < session.tx_end) {
        if (flush() < 0) return -1;
        if (session.tx_start == session.tx_end) break;
        int left = (int)((deadline - now()) * 1000);
        struct pollfd p = { KVM_TRANSPORT_FD, POLLOUT, 0 };
        if (left <= 0 || (poll(&p, 1, left) < 0 && errno != EINTR)) return -1;
    }
    return 0;
}

static int send_cancel(void)
{
    static const char cancel[] = "{\"type\":\"cancel\",\"scope\":\"all\"}";
    if (!session.hello) return 0; /* nothing can have been sent */
    return enqueue(cancel, sizeof cancel - 1) == 0 && flush_all() == 0 ? 0 : -1;
}

static int send_frame(const uint32_t *px, uint32_t w, uint32_t h)
{
    size_t size;
    if (kvm_dimensions(w, h, &size) < 0) return -1;
    int fd = memfd_create("kvm-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    void *dest = ftruncate(fd, (off_t)size) == 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                                                  : MAP_FAILED;
    if (dest == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memcpy(dest, px, size);
    munmap(dest, size);
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    struct kvm_ipc m = { .magic = KVM_IPC_MAGIC, .op = KVM_IPC_FRAME, .width = w, .height = h, .bytes = size };
    struct iovec iov = { &m, sizeof m };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } c;
    memset(&c, 0, sizeof c);
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = c.bytes, .msg_controllen = sizeof c.bytes };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof fd);
    memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    ssize_t n = sendmsg(KVM_IPC_FD, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
    if (n == (ssize_t)sizeof m) {
        session.frame_pending = 1;
        session.latest = NULL;
        return 0;
    }
    /* A full channel keeps the picture for the next chance; not an error. */
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

static int pixels(void *unused, const uint32_t *px, uint32_t w, uint32_t h)
{
    (void)unused;
    session.latest = px; /* the decoder's buffer: valid until its next picture */
    session.latest_w = w;
    session.latest_h = h;
    return session.frame_pending ? 0 : send_frame(px, w, h);
}

static int receive(void *unused, const unsigned char *data, size_t n)
{
    (void)unused;
    struct kvm_control c;
    if (!session.hello || kvm_control_parse(data, n, &c) < 0) return -1;
    session.last_rx = now();
    switch (c.type) {
    case KVM_MSG_WELCOME:
        if (session.welcomed) return -1;
        session.welcomed = 1;
        break;
    case KVM_MSG_CONFIG:
        if (!session.welcomed) return -1;
        session.active = 0;
        session.latest = NULL;
        kvm_decoder_close(session.decoder);
        if (!(session.decoder = kvm_decoder_open(c.codec, c.width, c.height))) return -1;
        session.active = 1;
        return status(KVM_IPC_ACTIVE);
    case KVM_MSG_VIDEO:
        if (!session.active) return -1; /* video before streamConfig */
        return kvm_decoder_push(session.decoder, data + c.video_offset, n - c.video_offset, pixels, NULL);
    case KVM_MSG_PING: {
        char s[96];
        int len = snprintf(s, sizeof s, "{\"type\":\"pong\",\"t\":%lld}", (long long)c.timestamp);
        return enqueue(s, (size_t)len);
    }
    case KVM_MSG_CLOSING:
        return -1;
    default:
        break; /* unknown types are ignored (OpenDisplay 6) */
    }
    return 0;
}

static int landlock(void)
{
    struct landlock_ruleset_attr rules = { .handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM };
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    int fd = (int)syscall(SYS_landlock_create_ruleset, &rules, sizeof rules, 0);
    if (fd < 0) return -1;
    int r = (int)syscall(SYS_landlock_restrict_self, fd, 0);
    close(fd);
    return r;
}

/* No new sockets, connections, programs, processes, threads or signals to
 * others: the two inherited AF_UNIX descriptors are all it talks through. */
static int seccomp(void)
{
#define DENY(call) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##call, 0, 1), \
                   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
#if defined(__x86_64__)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#elif defined(__aarch64__)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#else
#error "kvm-worker: no seccomp filter for this architecture"
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#if defined(__x86_64__)
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1), /* x32 */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
        DENY(socket), DENY(socketpair), DENY(connect), DENY(bind), DENY(listen),
        DENY(accept), DENY(accept4), DENY(execve), DENY(execveat), DENY(clone), DENY(clone3),
#ifdef SYS_fork
        DENY(fork), DENY(vfork),
#endif
        DENY(ptrace), DENY(kill), DENY(tkill), DENY(tgkill), DENY(pidfd_open),
        DENY(process_vm_readv), DENY(process_vm_writev),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
#undef DENY
    struct sock_fprog program = { .len = sizeof filter / sizeof filter[0], .filter = filter };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

static int sandbox(void)
{
    /* The FFmpeg decoder at 4096x4096 with its references, the picture and
     * the sealed copy: well under 1.5 GiB. */
    struct rlimit memory = { 1536ul << 20, 1536ul << 20 }, files = { 16, 16 }, none = { 0, 0 },
                  frame = { 64ul << 20, 64ul << 20 };
    if (!getuid() || getuid() != geteuid() || getgid() != getegid() || getgroups(0, NULL) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 || prctl(PR_SET_DUMPABLE, 0) < 0 ||
        setrlimit(RLIMIT_AS, &memory) < 0 || setrlimit(RLIMIT_NOFILE, &files) < 0 ||
        setrlimit(RLIMIT_CORE, &none) < 0 || setrlimit(RLIMIT_NPROC, &none) < 0 ||
        setrlimit(RLIMIT_FSIZE, &frame) < 0) {
        return -1;
    }
    return landlock() == 0 && seccomp() == 0 ? 0 : -1;
}

static int channel(int fd, int expected)
{
    int type, flags = fcntl(fd, F_GETFL);
    socklen_t size = sizeof type;
    struct sockaddr_storage addr;
    socklen_t length = sizeof addr;
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == expected &&
           getsockname(fd, (struct sockaddr *)&addr, &length) == 0 && addr.ss_family == AF_UNIX &&
           flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

/* One message from kvm: 0, 1 to end (STOP), -1 on a broken channel. */
static int frontend(void)
{
    struct kvm_ipc m;
    /* recv, not recvmsg: a descriptor from the frontend is never taken. */
    ssize_t n = recv(KVM_IPC_FD, &m, sizeof m, MSG_TRUNC | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n != (ssize_t)sizeof m || m.magic != KVM_IPC_MAGIC || m.reserved || m.length > KVM_IPC_TEXT || m.bytes) return -1;
    switch (m.op) {
    case KVM_IPC_STOP:
        send_cancel();
        return 1;
    case KVM_IPC_CANCEL:
        /* Answered only once the cancel is in the stream; else the stream
         * ends here, which the peer treats as a release as well. */
        return send_cancel() == 0 && status(KVM_IPC_CANCELLED) == 0 ? 0 : -1;
    case KVM_IPC_FRAME_ACK:
        session.frame_pending = 0;
        return session.latest ? send_frame(session.latest, session.latest_w, session.latest_h) : 0;
    case KVM_IPC_HELLO:
        if (session.hello || !m.length || enqueue(m.text, m.length) < 0) return -1;
        session.hello = 1;
        session.last_rx = now();
        return 0;
    case KVM_IPC_INPUT:
        if (!m.length) return -1;
        if (!session.active) return 0;
        /* Pointer motion is a rate, not a record: when the peer lags, the next
         * position replaces the backlog instead of ending the stream. */
        if (m.length >= 17 && memcmp(m.text, "{\"type\":\"pointer\"", 17) == 0 &&
            session.tx_end - session.tx_start > POINTER_SHED) {
            return 0;
        }
        return enqueue(m.text, m.length);
    default:
        return -1;
    }
}

int main(void)
{
    if (channel(KVM_IPC_FD, SOCK_SEQPACKET) < 0 || channel(KVM_TRANSPORT_FD, SOCK_STREAM) < 0 || sandbox() < 0 ||
        (!kvm_decoder_available(KVM_CODEC_H264) && !kvm_decoder_available(KVM_CODEC_HEVC))) {
        status(KVM_IPC_UNAVAILABLE);
        return 78;
    }
    if (status(KVM_IPC_READY) < 0) return 1;
    session.last_rx = session.last_ping = now();
    for (;;) {
        struct pollfd p[2] = {
            { KVM_IPC_FD, POLLIN, 0 },
            { KVM_TRANSPORT_FD, POLLIN | (session.tx_end > session.tx_start ? POLLOUT : 0), 0 },
        };
        int r = poll(p, 2, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) || (p[1].revents & (POLLERR | POLLNVAL))) break;
        if ((p[0].revents & POLLIN) && (r = frontend()) != 0) break;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            unsigned char data[16384];
            ssize_t n = recv(KVM_TRANSPORT_FD, data, sizeof data, MSG_DONTWAIT);
            if (n == 0) break; /* the peer or the broker ended the stream */
            if (n < 0 && errno != EAGAIN && errno != EINTR) break;
            if (n > 0 && kvm_parser_feed(&session.parser, data, (size_t)n, receive, NULL) < 0) break;
        }
        double t = now();
        if (t - session.last_rx > TIMEOUT_S) break;
        if (session.hello && t - session.last_ping >= PING_S) {
            char ping[96];
            int n = snprintf(ping, sizeof ping, "{\"type\":\"ping\",\"t\":%lld}", (long long)(t * 1000));
            if (enqueue(ping, (size_t)n) < 0) break;
            session.last_ping = t;
        }
        if (flush() < 0) break;
    }
    status(KVM_IPC_FAILED);
    kvm_decoder_close(session.decoder);
    kvm_parser_clear(&session.parser);
    return 1;
}
