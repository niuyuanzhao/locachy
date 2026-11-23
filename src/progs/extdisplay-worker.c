/*
 * extdisplay-worker: the extended-display receiver's untrusted half (extdisplay-ipc.h).
 * It alone reads the paired stream: framing, JSON and video (distribution
 * FFmpeg, extdisplay-decode.c). It runs as nobody with no groups, no new
 * privileges, a Landlock ruleset that grants no path, resource limits and a
 * seccomp filter against new sockets, processes and signals; it refuses to
 * run without them. Pictures go to extdisplay as sealed memfds, one in flight; a
 * newer picture replaces an unsent one, never queues behind it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-ipc.h"
#include "extdisplay-decode.h"
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

#define TX_MAX (256u * 1024u)
#define POINTER_SHED (32u * 1024u) /* pending bytes above which pointer motion is dropped */
#define DECODE_ERRORS_MAX 5        /* consecutive recoverable decode errors before failing */
#define TIMEOUT_S 8.0      /* nothing from the peer: it is gone */
#define PING_S 2.0         /* OpenDisplay's cadence */
#define FLUSH_MS 100       /* a cancel is written within this, or the stream closes */

static struct {
    struct extdisplay_decoder *decoder;
    struct extdisplay_parser parser;
    unsigned char tx[TX_MAX];
    size_t tx_start, tx_end;
    double last_rx, last_ping;
    int hello, welcomed, active, frame_pending, dmabuf_off;
    int decode_errors;
    unsigned char slot_busy[EXTDISPLAY_DMABUF_SLOTS];
    unsigned char slot_exported[EXTDISPLAY_DMABUF_SLOTS];
    int ready_slot, last_sent_slot;
    uint32_t width, height;
    unsigned hw_caps;
    uint32_t input_caps;
} session = { .ready_slot = -1, .last_sent_slot = -1 };

static const char *fail_reason = "unknown";

static int fail_with(const char *why)
{
    fail_reason = why;
    return -1;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int status_reason(uint32_t op, const char *reason)
{
    struct extdisplay_ipc m = { .magic = EXTDISPLAY_IPC_MAGIC, .op = op };
    if (op == EXTDISPLAY_IPC_READY) {
        m.width = (extdisplay_decoder_available(EXTDISPLAY_CODEC_H264) ? 1u : 0u) |
                  (extdisplay_decoder_available(EXTDISPLAY_CODEC_HEVC) ? 2u : 0u) |
                  ((session.hw_caps & 7u) << 2);
    } else if (op == EXTDISPLAY_IPC_ACTIVE) {
        m.width = session.input_caps;
    } else if (op == EXTDISPLAY_IPC_FAILED && reason && reason[0]) {
        size_t len = strlen(reason);
        if (len > EXTDISPLAY_IPC_TEXT) len = EXTDISPLAY_IPC_TEXT;
        memcpy(m.text, reason, len);
        m.length = (uint32_t)len;
    }
    return send(EXTDISPLAY_IPC_FD, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof m ? 0 : -1;
}

static int status(uint32_t op)
{
    return status_reason(op, NULL);
}

static int enqueue(const void *data, size_t n)
{
    if (!n || n > EXTDISPLAY_CONTROL_MAX) return fail_with("tx-overflow");
    if (session.tx_start) {
        memmove(session.tx, session.tx + session.tx_start, session.tx_end - session.tx_start);
        session.tx_end -= session.tx_start;
        session.tx_start = 0;
    }
    if (n + 4 > TX_MAX - session.tx_end) return fail_with("tx-overflow"); /* a peer that reads nothing */
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
        ssize_t n = send(EXTDISPLAY_TRANSPORT_FD, session.tx + session.tx_start, session.tx_end - session.tx_start,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return fail_with("tx-write-err");
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
        struct pollfd p = { EXTDISPLAY_TRANSPORT_FD, POLLOUT, 0 };
        if (left <= 0 || (poll(&p, 1, left) < 0 && errno != EINTR)) return fail_with("tx-write-err");
    }
    return 0;
}

static int send_cancel(void)
{
    static const char cancel[] = "{\"type\":\"cancel\",\"scope\":\"all\"}";
    if (!session.hello) return 0; /* nothing can have been sent */
    return enqueue(cancel, sizeof cancel - 1) == 0 && flush_all() == 0 ? 0 : -1;
}

static int send_fds_msg(struct extdisplay_ipc *m, const int *fds, unsigned nfds)
{
    struct iovec iov = { m, sizeof *m };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(EXTDISPLAY_DMABUF_MAX_PLANES * sizeof(int))]; } c;
    memset(&c, 0, sizeof c);
    if (!nfds || nfds > EXTDISPLAY_DMABUF_MAX_PLANES) return -1;
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = c.bytes, .msg_controllen = CMSG_SPACE(nfds * sizeof(int)),
    };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(nfds * sizeof(int));
    memcpy(CMSG_DATA(cm), fds, nfds * sizeof(int));
    ssize_t n = sendmsg(EXTDISPLAY_IPC_FD, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    for (unsigned i = 0; i < nfds; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    if (n == (ssize_t)sizeof *m) {
        session.frame_pending = 1;
        return 0;
    }
    /* A full channel keeps the loop going; not an error. */
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

static int send_fd_msg(struct extdisplay_ipc *m, int fd)
{
    return send_fds_msg(m, &fd, 1);
}

static void reset_dmabuf_slots(void)
{
    memset(session.slot_busy, 0, sizeof session.slot_busy);
    memset(session.slot_exported, 0, sizeof session.slot_exported);
    session.ready_slot = -1;
    session.last_sent_slot = -1;
}

static int prepare_dmabuf_slot(void)
{
    if (session.dmabuf_off || !session.decoder) return 0;
    int slot = session.ready_slot;
    if (slot < 0) {
        for (int s = 0; s < (int)EXTDISPLAY_DMABUF_SLOTS; s++) {
            if (!session.slot_busy[s]) {
                slot = s;
                break;
            }
        }
    }
    if (slot < 0) return -1; /* all slots currently owned by frontend; wait for ACK */
    if (extdisplay_decoder_prepare_dmabuf(session.decoder, (uint32_t)slot) <= 0) {
        session.ready_slot = -1;
        return 0;
    }
    session.ready_slot = slot;
    return 1;
}

static int send_latest(void);

static int send_ready_slot(void)
{
    if (session.frame_pending || session.ready_slot < 0 || session.dmabuf_off) return 0;
    int s = session.ready_slot;
    uint32_t gen = extdisplay_decoder_dmabuf_gen(session.decoder);
    if (!gen) {
        session.ready_slot = -1;
        return 0;
    }
    if (!session.slot_exported[s]) {
        int dmabuf_fds[EXTDISPLAY_DMABUF_MAX_PLANES] = { -1, -1, -1, -1 };
        struct extdisplay_dmabuf info;
        if (extdisplay_decoder_export_slot(session.decoder, (uint32_t)s, dmabuf_fds, &info) <= 0 ||
            info.num_objects < 1 || info.num_objects > EXTDISPLAY_DMABUF_MAX_PLANES) {
            for (unsigned i = 0; i < EXTDISPLAY_DMABUF_MAX_PLANES; i++) {
                if (dmabuf_fds[i] >= 0) close(dmabuf_fds[i]);
            }
            session.dmabuf_off = 1;
            reset_dmabuf_slots();
            return send_latest();
        }
        struct extdisplay_ipc m = {
            .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_DMABUF,
            .width = session.width, .height = session.height, .length = sizeof info,
        };
        memcpy(m.text, &info, sizeof info);
        if (send_fds_msg(&m, dmabuf_fds, info.num_objects) < 0) return -1;
        if (session.frame_pending) {
            session.ready_slot = -1;
            session.slot_exported[s] = 1;
            session.slot_busy[s] = 1;
            session.last_sent_slot = s;
        }
        return 0;
    }
    struct extdisplay_dmabuf info = {
        .slot = (uint32_t)s,
        .generation = gen,
    };
    struct extdisplay_ipc m = {
        .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_DMABUF,
        .width = session.width, .height = session.height, .length = sizeof info,
    };
    memcpy(m.text, &info, sizeof info);
    ssize_t n = send(EXTDISPLAY_IPC_FD, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n == (ssize_t)sizeof m) {
        session.frame_pending = 1;
        session.ready_slot = -1;
        session.slot_busy[s] = 1;
        session.last_sent_slot = s;
        return 0;
    }
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

static int send_latest(void)
{
    uint32_t w, h;
    size_t size;
    if (session.frame_pending) return 0;
    if (session.ready_slot >= 0) return send_ready_slot();
    if (!extdisplay_decoder_size(session.decoder, &w, &h)) return 0;
    if (!session.dmabuf_off) {
        int prep = prepare_dmabuf_slot();
        if (prep > 0) return send_ready_slot();
        if (prep < 0) return 0;
    }
    if (extdisplay_dimensions(w, h, &size) < 0) return -1;
    int fd = memfd_create("extdisplay-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    uint32_t *dest = ftruncate(fd, (off_t)size) == 0
                         ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                         : MAP_FAILED;
    if (dest == MAP_FAILED) {
        close(fd);
        return -1;
    }
    (void)madvise(dest, size, MADV_HUGEPAGE);
    int rendered = extdisplay_decoder_render(session.decoder, dest);
    munmap(dest, size);
    if (rendered < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    struct extdisplay_ipc m = { .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_FRAME, .width = w, .height = h, .bytes = size };
    return send_fd_msg(&m, fd);
}

static int receive(void *unused, const unsigned char *data, size_t n)
{
    (void)unused;
    struct extdisplay_control c;
    if (!session.hello || extdisplay_control_parse(data, n, &c) < 0) return fail_with("parse-err");
    session.last_rx = now();
    switch (c.type) {
    case EXTDISPLAY_MSG_WELCOME:
        if (session.welcomed) return fail_with("parse-err");
        session.welcomed = 1;
        session.input_caps = c.input_caps;
        break;
    case EXTDISPLAY_MSG_CONFIG:
        if (!session.welcomed) return fail_with("config-err");
        session.active = 0;
        session.frame_pending = 0;
        session.decode_errors = 0;
        reset_dmabuf_slots();
        extdisplay_decoder_close(session.decoder);
        if (!(session.decoder = extdisplay_decoder_open(c.codec, c.width, c.height))) return fail_with("config-err");
        if (session.dmabuf_off) extdisplay_decoder_disable_dmabuf(session.decoder);
        session.width = c.width;
        session.height = c.height;
        session.active = 1;
        return status(EXTDISPLAY_IPC_ACTIVE) < 0 ? fail_with("frontend-err") : 0;
    case EXTDISPLAY_MSG_VIDEO: {
        if (!session.active) return fail_with("parse-err"); /* video before streamConfig */
        int r = extdisplay_decoder_step(session.decoder, data + c.video_offset, n - c.video_offset);
        if (r < 0) {
            if (++session.decode_errors <= DECODE_ERRORS_MAX) {
                static const char kf[] = "{\"type\":\"kf\"}";
                if (enqueue(kf, sizeof kf - 1) < 0 || flush() < 0) return -1;
                return 0;
            }
            return fail_with("video-decode-err");
        }
        if (r > 0) {
            session.decode_errors = 0;
            if (!session.dmabuf_off) {
                int prep = prepare_dmabuf_slot();
                if (prep > 0) return !session.frame_pending && send_ready_slot() < 0 ? fail_with("frontend-err") : 0;
                if (prep < 0) return 0;
            }
            return !session.frame_pending && send_latest() < 0 ? fail_with("frontend-err") : 0;
        }
        return 0;
    }
    case EXTDISPLAY_MSG_PING: {
        char s[96];
        int len = snprintf(s, sizeof s, "{\"type\":\"pong\",\"t\":%lld}", (long long)c.timestamp);
        if (enqueue(s, (size_t)len) < 0) return -1;
        return flush();
    }
    case EXTDISPLAY_MSG_CLOSING:
        return fail_with("peer-closing");
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
#error "extdisplay-worker: no seccomp filter for this architecture"
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

static int sandbox(int hw)
{
    /* The FFmpeg decoder at 4096x4096 with VA-API/FFmpeg surface pools, the
     * picture and the sealed copy: 2.5 GiB virtual address headroom. VA-API
     * keeps DRM surface fds open. */
    struct rlimit memory = { 2560ul << 20, 2560ul << 20 }, files = { hw ? 64 : 16, hw ? 64 : 16 },
                  none = { 0, 0 }, frame = { 64ul << 20, 64ul << 20 };
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

/* Drain queued messages from extdisplay: 0, 1 to end (STOP), -1 on a broken channel. */
static int frontend(void)
{
    for (int count = 0; count < 128; count++) {
        struct extdisplay_ipc m;
        /* recv, not recvmsg: a descriptor from the frontend is never taken. */
        ssize_t n = recv(EXTDISPLAY_IPC_FD, &m, sizeof m, MSG_TRUNC | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n != (ssize_t)sizeof m || m.magic != EXTDISPLAY_IPC_MAGIC || m.reserved || m.length > EXTDISPLAY_IPC_TEXT || m.bytes) {
            return fail_with("frontend-err");
        }
        switch (m.op) {
        case EXTDISPLAY_IPC_STOP:
            send_cancel();
            fail_reason = "frontend-stop";
            return 1;
        case EXTDISPLAY_IPC_CANCEL:
            /* Answered only once the cancel is in the stream; else the stream
             * ends here, which the peer treats as a release as well. */
            if (send_cancel() != 0) return -1;
            if (status(EXTDISPLAY_IPC_CANCELLED) != 0) return fail_with("frontend-err");
            break;
        case EXTDISPLAY_IPC_FRAME_ACK:
            session.frame_pending = 0;
            if (m.width == EXTDISPLAY_DMABUF_RELEASE_ALL) {
                memset(session.slot_busy, 0, sizeof session.slot_busy);
            } else if (m.width >= 1 && m.width <= EXTDISPLAY_DMABUF_SLOTS) {
                session.slot_busy[m.width - 1] = 0;
            }
            if (session.ready_slot >= 0) {
                if (send_ready_slot() < 0) return fail_with("frontend-err");
            } else if (send_latest() < 0) {
                return fail_with("frontend-err");
            }
            break;
        case EXTDISPLAY_IPC_DMABUF_OFF:
            session.dmabuf_off = 1;
            reset_dmabuf_slots();
            extdisplay_decoder_disable_dmabuf(session.decoder);
            break;
        case EXTDISPLAY_IPC_REFRESH: {
            if (!session.active || session.dmabuf_off) break;
            int target = session.ready_slot >= 0 ? session.ready_slot : session.last_sent_slot;
            reset_dmabuf_slots();
            session.frame_pending = 0;
            if (m.width >= 1 && m.width <= EXTDISPLAY_DMABUF_SLOTS && (int)(m.width - 1) != target) {
                session.slot_busy[m.width - 1] = 1;
            }
            if (target >= 0) {
                session.ready_slot = target;
                if (send_ready_slot() < 0) return fail_with("frontend-err");
            }
            break;
        }
        case EXTDISPLAY_IPC_HELLO:
            if (session.hello || !m.length) return fail_with("frontend-err");
            if (enqueue(m.text, m.length) < 0) return -1;
            session.hello = 1;
            session.last_rx = now();
            break;
        case EXTDISPLAY_IPC_INPUT:
            if (!m.length) return fail_with("frontend-err");
            if (!session.active) break;
            /* Rate-shed pointer/tablet motion and move-only touch/scroll updates
             * when the peer lags so a transient stall never overflows tx. */
            if (session.tx_end - session.tx_start > POINTER_SHED &&
                extdisplay_input_droppable(m.text, m.length)) {
                break;
            }
            if (enqueue(m.text, m.length) < 0) return -1;
            break;
        default:
            return fail_with("frontend-err");
        }
    }
    return 0;
}

int main(void)
{
    int render_fd = fcntl(EXTDISPLAY_RENDER_FD, F_GETFD) >= 0 ? EXTDISPLAY_RENDER_FD : -1;
    if (channel(EXTDISPLAY_IPC_FD, SOCK_SEQPACKET) < 0 || channel(EXTDISPLAY_TRANSPORT_FD, SOCK_STREAM) < 0) {
        if (render_fd >= 0) close(render_fd);
        status(EXTDISPLAY_IPC_UNAVAILABLE);
        return 78;
    }
    if (render_fd >= 0 && extdisplay_vaapi_init(render_fd, &session.hw_caps) < 0) {
        close(render_fd);
        render_fd = -1;
    }
    if (sandbox(render_fd >= 0) < 0 ||
        (!extdisplay_decoder_available(EXTDISPLAY_CODEC_H264) && !extdisplay_decoder_available(EXTDISPLAY_CODEC_HEVC))) {
        status(EXTDISPLAY_IPC_UNAVAILABLE);
        return 78;
    }
    if (status(EXTDISPLAY_IPC_READY) < 0) return 1;
    session.last_rx = session.last_ping = now();
    for (;;) {
        int ipc_want_out = !session.frame_pending && session.active &&
                           (session.ready_slot >= 0 ||
                            (session.dmabuf_off && extdisplay_decoder_size(session.decoder, NULL, NULL)));
        struct pollfd p[2] = {
            { EXTDISPLAY_IPC_FD, (short)(POLLIN | (ipc_want_out ? POLLOUT : 0)), 0 },
            { EXTDISPLAY_TRANSPORT_FD, POLLIN | (session.tx_end > session.tx_start ? POLLOUT : 0), 0 },
        };
        int r = poll(p, 2, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { fail_reason = "transport-poll-err"; break; }
        if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) { fail_reason = "ipc-poll-err"; break; }
        if (p[1].revents & (POLLERR | POLLNVAL)) { fail_reason = "transport-poll-err"; break; }
        if ((p[0].revents & POLLIN) && (r = frontend()) != 0) break;
        if ((p[0].revents & POLLOUT) && !session.frame_pending && session.active) {
            if (session.ready_slot >= 0) {
                if (send_ready_slot() < 0) { fail_reason = "frontend-err"; break; }
            } else if (session.dmabuf_off && send_latest() < 0) {
                fail_reason = "frontend-err";
                break;
            }
        }
        /* Flush outbound input/control to the broker before spending time decoding inbound video. */
        if (session.tx_end > session.tx_start && flush() < 0) break;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            int ended = 0;
            for (int reads = 0; reads < 8; reads++) {
                unsigned char data[65536];
                ssize_t n = recv(EXTDISPLAY_TRANSPORT_FD, data, sizeof data, MSG_DONTWAIT);
                if (n == 0) { fail_reason = "transport-eof"; ended = 1; break; } /* the peer or the broker ended the stream */
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    fail_reason = "transport-read-err";
                    ended = 1;
                    break;
                }
                session.last_rx = now();
                fail_reason = "parse-err";
                if (extdisplay_parser_feed(&session.parser, data, (size_t)n, receive, NULL) < 0) {
                    ended = 1;
                    break;
                }
                fail_reason = "unknown";
                if (session.tx_end > session.tx_start && flush() < 0) {
                    ended = 1;
                    break;
                }
            }
            if (ended) break;
        }
        double t = now();
        if (t - session.last_rx > TIMEOUT_S) { fail_reason = "peer-timeout"; break; }
        if (session.hello && t - session.last_ping >= PING_S) {
            char ping[96];
            int n = snprintf(ping, sizeof ping, "{\"type\":\"ping\",\"t\":%lld}", (long long)(t * 1000));
            if (enqueue(ping, (size_t)n) < 0) break;
            session.last_ping = t;
        }
        if (flush() < 0) break;
    }
    status_reason(EXTDISPLAY_IPC_FAILED, fail_reason);
    extdisplay_decoder_close(session.decoder);
    extdisplay_parser_clear(&session.parser);
    return 1;
}
