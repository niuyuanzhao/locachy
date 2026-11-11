/* extdisplay-broker-pump.c: the child that meets the network. Forked by extdisplay-broker
 * for one attempt, it enters the session namespace, binds the listener to the
 * leased device's link-local address, prepares OpenSSL, and then gives up
 * everything before it reads a byte from the peer: nobody, no groups, no
 * capabilities (bounding and ambient sets emptied), no_new_privs, not
 * dumpable, resource limits, a Landlock ruleset that grants no path and no
 * TCP bind or connect, and a seccomp filter against new sockets, programs,
 * processes, namespaces, mounts and signals. One connection is accepted; the
 * handshake must be the cable profile; then it pumps bytes until either side
 * ends. It dies with the broker (parent-death signal), so it can never keep
 * the session namespace alive on its own. */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <netinet/tcp.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "extdisplay-broker.h"

#ifndef SO_BINDTOIFINDEX
#define SO_BINDTOIFINDEX 62
#endif

enum { PUMP_NETNS = 3, PUMP_PLAIN = 4, PUMP_STATUS = 5 };

static void status_send(const char *text)
{
    (void)!send(PUMP_STATUS, text, strlen(text), MSG_NOSIGNAL);
}

static void __attribute__((noreturn)) fail(const char *what)
{
    char line[128];
    snprintf(line, sizeof line, "error %s", what);
    status_send(line);
    _exit(1);
}

/* The three descriptors at fixed numbers, everything else closed: the
 * broker's control socket, home namespace, netlink sockets and state lock
 * never reach the network side. */
static int keep_descriptors(const struct pump_args *a)
{
    int ns = fcntl(a->netns, F_DUPFD_CLOEXEC, 100);
    int plain = fcntl(a->plain, F_DUPFD_CLOEXEC, 100);
    int status = fcntl(a->status, F_DUPFD_CLOEXEC, 100);
    if (ns < 0 || plain < 0 || status < 0 || dup3(ns, PUMP_NETNS, O_CLOEXEC) < 0 ||
        dup3(plain, PUMP_PLAIN, O_CLOEXEC) < 0 || dup3(status, PUMP_STATUS, O_CLOEXEC) < 0) {
        return -1;
    }
    return (int)syscall(SYS_close_range, PUMP_STATUS + 1, ~0U, 0);
}

static int make_listener(int ifindex, const struct in6_addr *address)
{
    struct sockaddr_in6 local = { .sin6_family = AF_INET6, .sin6_port = htons(EXTDISPLAY_BROKER_PORT),
                                  .sin6_addr = *address, .sin6_scope_id = (uint32_t)ifindex };
    int one = 1, fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BINDTOIFINDEX, &ifindex, sizeof ifindex) < 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof local) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int no_capabilities(void)
{
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof data);
    if (syscall(SYS_capget, &header, data) < 0) return 0;
    for (int i = 0; i < 2; i++)
        if (data[i].effective || data[i].permitted || data[i].inheritable) return 0;
    return 1;
}

static int drop_privileges(const struct pump_args *a)
{
    uid_t ru, eu, su;
    gid_t rg, eg, sg;
    if (!a->uid || !a->gid) return -1;
    for (int cap = 0; cap < 64; cap++) {
        if (prctl(PR_CAPBSET_READ, cap, 0, 0, 0) < 0) break;
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) return -1;
    }
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0) return -1;
    if (setgroups(0, NULL) < 0 || setresgid(a->gid, a->gid, a->gid) < 0 || setresuid(a->uid, a->uid, a->uid) < 0)
        return -1;
    if (getresuid(&ru, &eu, &su) < 0 || ru != a->uid || eu != a->uid || su != a->uid ||
        getresgid(&rg, &eg, &sg) < 0 || rg != a->gid || eg != a->gid || sg != a->gid || getgroups(0, NULL) != 0 ||
        !no_capabilities()) {
        return -1;
    }
    /* A credential change clears the parent-death signal: set it again and
     * make sure the broker did not die in between. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != a->parent) return -1;
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
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
    if (abi >= 4) rules.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
#ifdef LANDLOCK_SCOPE_SIGNAL
    if (abi >= 6) rules.scoped = LANDLOCK_SCOPE_SIGNAL | LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET;
#endif
    int fd = (int)syscall(SYS_landlock_create_ruleset, &rules, sizeof rules, 0);
    if (fd < 0) return -1;
    int r = (int)syscall(SYS_landlock_restrict_self, fd, 0);
    close(fd);
    return r;
}

#define DENY(call) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##call, 0, 1), \
                   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
#if defined(__x86_64__)
#define FILTER_HEAD \
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), \
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)), \
    BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
#elif defined(__aarch64__)
#define FILTER_HEAD \
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), \
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#else
#error "extdisplay-broker: no seccomp filter for this architecture"
#endif

static int install(struct sock_filter *filter, unsigned short length)
{
    struct sock_fprog program = { .len = length, .filter = filter };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

/* The listener exists; the pump needs accept once, then only reads,
 * writes, poll and memory. */
static int seccomp_base(void)
{
    struct sock_filter filter[] = {
        FILTER_HEAD,
        DENY(socket), DENY(socketpair), DENY(connect), DENY(bind), DENY(listen),
        DENY(execve), DENY(execveat), DENY(clone), DENY(clone3),
#ifdef SYS_fork
        DENY(fork), DENY(vfork),
#endif
        DENY(ptrace), DENY(kill), DENY(tkill), DENY(tgkill), DENY(pidfd_open), DENY(pidfd_send_signal),
        DENY(process_vm_readv), DENY(process_vm_writev), DENY(setns), DENY(unshare), DENY(mount),
        DENY(umount2), DENY(pivot_root), DENY(chroot), DENY(open_tree), DENY(move_mount), DENY(fsopen),
        DENY(fsmount), DENY(fsconfig), DENY(fspick), DENY(open_by_handle_at), DENY(name_to_handle_at),
        DENY(io_uring_setup), DENY(io_uring_enter), DENY(io_uring_register), DENY(bpf), DENY(perf_event_open),
        DENY(userfaultfd), DENY(keyctl), DENY(add_key), DENY(request_key), DENY(memfd_create),
        DENY(setuid), DENY(setgid), DENY(setreuid), DENY(setregid), DENY(setresuid), DENY(setresgid),
        DENY(setgroups), DENY(capset), DENY(sendmsg), DENY(sendmmsg), DENY(recvmsg), DENY(recvmmsg),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    return install(filter, sizeof filter / sizeof filter[0]);
}

static int seccomp_accepted(void)
{
    struct sock_filter filter[] = {
        FILTER_HEAD,
        DENY(accept), DENY(accept4),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    return install(filter, sizeof filter / sizeof filter[0]);
}

static int sandbox(void)
{
    struct rlimit memory = { 256ul << 20, 256ul << 20 }, files = { 8, 8 }, none = { 0, 0 };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        prctl(PR_SET_DUMPABLE, 0) < 0 || setrlimit(RLIMIT_AS, &memory) < 0 || setrlimit(RLIMIT_NOFILE, &files) < 0 ||
        setrlimit(RLIMIT_CORE, &none) < 0 || setrlimit(RLIMIT_NPROC, &none) < 0 ||
        setrlimit(RLIMIT_FSIZE, &none) < 0 || setrlimit(RLIMIT_MEMLOCK, &none) < 0) {
        return -1;
    }
    return landlock() == 0 && seccomp_base() == 0 ? 0 : -1;
}

#ifdef EXTDISPLAY_BROKER_TEST_PROBES
/* Test build only: what the sandboxed pump can still do, reported to the
 * broker, which logs it. */
static void probes(void)
{
    char line[256];
    int s = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int ns = open("/proc/1/ns/net", O_RDONLY | O_CLOEXEC);
    int file = open("/etc/hostname", O_RDONLY | O_CLOEXEC);
    pid_t child = fork();
    if (child == 0) _exit(0);
    if (child > 0) waitpid(child, NULL, 0);
    int root = setuid(0) == 0;
    int share = unshare(CLONE_NEWNET) == 0;
    snprintf(line, sizeof line, "probe socket=%s open-netns=%s open-file=%s fork=%s setuid0=%s unshare=%s caps=%s uid=%u",
             s < 0 ? "denied" : "allowed", ns < 0 ? "denied" : "allowed", file < 0 ? "denied" : "allowed",
             child < 0 ? "denied" : "allowed", root ? "allowed" : "denied", share ? "allowed" : "denied",
             no_capabilities() ? "none" : "some", (unsigned)getuid());
    if (s >= 0) close(s);
    if (ns >= 0) close(ns);
    if (file >= 0) close(file);
    status_send(line);
}
#endif

void pump_main(struct pump_args *a)
{
    sigset_t none;
    SSL_CTX *ctx;
    SSL *ssl;
    int listener, tcp, one = 1;
    unsigned char warm[16];
    struct sockaddr_in6 peer;
    socklen_t peer_len = sizeof peer;

    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != a->parent) _exit(1);
    if (keep_descriptors(a) < 0) _exit(1);
    if (setns(PUMP_NETNS, CLONE_NEWNET) < 0) fail("cannot enter the session namespace");
    close(PUMP_NETNS);
    if ((listener = make_listener(a->ifindex, &a->linklocal)) < 0) fail("cannot listen on the link-local address");
    if (extdisplay_broker_tls_init() < 0 || !(ctx = extdisplay_broker_tls_context(&a->key)) || !(ssl = SSL_new(ctx)) ||
        RAND_bytes(warm, sizeof warm) != 1) {
        fail("TLS setup failed");
    }
    OPENSSL_cleanse(warm, sizeof warm);
    if (drop_privileges(a) < 0 || sandbox() < 0) fail("sandbox unavailable");
#ifdef EXTDISPLAY_BROKER_TEST_PROBES
    probes();
#endif
    status_send("listening");

    for (;;) {
        long long left = a->deadline_ms - extdisplay_broker_now_ms();
        struct pollfd p = { listener, POLLIN, 0 };
        if (left <= 0) {
            status_send("timeout");
            _exit(0);
        }
        int n = poll(&p, 1, left > 60000 ? 60000 : (int)left);
        if (n < 0 && errno != EINTR) fail("poll");
        if (n <= 0) continue;
        tcp = accept4(listener, (struct sockaddr *)&peer, &peer_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (tcp >= 0) break;
        if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED) fail("accept");
    }
    close(listener);
    if (seccomp_accepted() < 0) fail("sandbox unavailable");
    if (peer.sin6_family != AF_INET6 || !IN6_IS_ADDR_LINKLOCAL(&peer.sin6_addr) ||
        peer.sin6_scope_id != (uint32_t)a->ifindex) {
        status_send("handshake-failed");
        _exit(0);
    }
    (void)setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* Abortive close: when this process dies (killed with the broker) the
     * connection is reset and freed at once, so no orphaned TCP socket
     * keeps the session namespace, and the leased device, alive. */
    struct linger abort_close = { 1, 0 };
    if (setsockopt(tcp, SOL_SOCKET, SO_LINGER, &abort_close, sizeof abort_close) < 0) fail("socket options");
    long long deadline = extdisplay_broker_now_ms() + EXTDISPLAY_BROKER_HANDSHAKE_SECONDS * 1000LL;
    if (deadline > a->deadline_ms) deadline = a->deadline_ms;
    int handshake = extdisplay_broker_tls_accept(ssl, tcp, deadline);
    OPENSSL_cleanse(&a->key, sizeof a->key);
    if (handshake < 0) {
        status_send(extdisplay_broker_now_ms() >= a->deadline_ms ? "timeout" : "handshake-failed");
        _exit(0);
    }
    status_send("paired");
    /* The broker shuts its end of the status socket down to stop the pump. */
    int pumped = extdisplay_broker_pump(ssl, tcp, PUMP_PLAIN, PUMP_STATUS);
    /* close_notify is queued: FIN after it, then up to 250 ms for the peer
     * to close too, so the reset of the abortive close cannot overtake it. */
    if (shutdown(tcp, SHUT_WR) == 0) {
        char discard[4096];
        for (long long end = extdisplay_broker_now_ms() + 250;;) {
            long long left = end - extdisplay_broker_now_ms();
            struct pollfd p = { tcp, POLLIN, 0 };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
            ssize_t n = read(tcp, discard, sizeof discard);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) break;
        }
    }
    status_send(pumped == 0 ? "closed" : "closed error");
    _exit(0);
}
