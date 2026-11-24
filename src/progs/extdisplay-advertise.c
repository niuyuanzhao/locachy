/* A keyless, IPv6-only Avahi-core responder in one leased session namespace.
 * No daemon, D-Bus, system hostname, unicast DNS or reflector is involved.
 * The broker execs this before it generates the TLS pairing key. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <avahi-common/address.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/timeval.h>
#include <avahi-core/core.h>
#include <avahi-core/log.h>
#include <avahi-core/publish.h>

#define STATUS 3
#define SERVICE "_locachy-ext._tcp"
#define PORT 27183

static AvahiSimplePoll *loop;
static AvahiSEntryGroup *group;
static int interface_index, failed, established, sandboxed, stopping;
static char service_name[40], host_name[48], code_txt[48], hostid_txt[48];
static int has_code, has_hostid, paired_only;
static AvahiAddress endpoint;

static int valid_host_id(const char *s, size_t n)
{
    if (n < 16 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int valid_code(const char *s, size_t n)
{
    if (n != 32) return 0; /* 26 symbols + 6 '-' */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i % 5 == 4) {
            if (c != '-') return 0;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) {
            return 0;
        }
    }
    return 1;
}

static void quiet_log(AvahiLogLevel level, const char *text)
{
    (void)level;
    (void)text; /* No untrusted network names or machine identity in logs. */
}

static void stop_error(void)
{
    failed = 1;
    if (loop) avahi_simple_poll_quit(loop);
}

static void group_event(AvahiServer *s, AvahiSEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
    (void)s;
    (void)g;
    (void)userdata;
    if (state == AVAHI_ENTRY_GROUP_ESTABLISHED) {
        established = 1;
        if (sandboxed && send(STATUS, "ready", 5, MSG_NOSIGNAL) != 5) stop_error();
    } else if (state == AVAHI_ENTRY_GROUP_COLLISION || state == AVAHI_ENTRY_GROUP_FAILURE) stop_error();
}

static void server_event(AvahiServer *s, AvahiServerState state, void *userdata)
{
    (void)userdata;
    if (state == AVAHI_SERVER_FAILURE || state == AVAHI_SERVER_COLLISION) {
        stop_error();
    } else if (state == AVAHI_SERVER_RUNNING && !group) {
        AvahiStringList *txt = NULL;
        txt = avahi_string_list_add(txt, "pv=3");
        txt = avahi_string_list_add(txt, "extended=1");
        txt = avahi_string_list_add(txt, "pair=psk128");
        if (has_code) txt = avahi_string_list_add(txt, code_txt);
        if (has_hostid) txt = avahi_string_list_add(txt, hostid_txt);
        if (paired_only) txt = avahi_string_list_add(txt, "paired=1");
        group = avahi_s_entry_group_new(s, group_event, NULL);
        if (!group || !txt || avahi_server_add_address(s, group, interface_index, AVAHI_PROTO_INET6, 0,
                                               host_name, &endpoint) < 0 ||
            avahi_server_add_service_strlst(s, group, interface_index, AVAHI_PROTO_INET6, 0,
                                            service_name, SERVICE, "local", host_name, PORT, txt) < 0 ||
            avahi_s_entry_group_commit(group) < 0) {
            avahi_string_list_free(txt);
            stop_error();
            return;
        }
        avahi_string_list_free(txt);
    }
}

static void owner_event(AvahiWatch *watch, int fd, AvahiWatchEvent events, void *userdata)
{
    char byte;
    (void)watch;
    (void)events;
    (void)userdata;
    /* A byte or EOF asks the responder to withdraw its records. */
    if (recv(fd, &byte, 1, MSG_DONTWAIT) >= 0 || errno != EAGAIN) stopping = 1;
}

static void goodbye_done(AvahiTimeout *timeout, void *userdata)
{
    (void)timeout;
    (void)userdata;
    avahi_simple_poll_quit(loop);
}

static int drop(uid_t uid, gid_t gid, pid_t parent)
{
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct caps[2] = { {0}, {0} };
    if (!uid || !gid) return -1;
    for (int cap = 0; cap < 64; cap++) {
        if (prctl(PR_CAPBSET_READ, cap, 0, 0, 0) < 0) break;
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) return -1;
    }
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0 ||
        setgroups(0, NULL) < 0 || setresgid(gid, gid, gid) < 0 || setresuid(uid, uid, uid) < 0 ||
        prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent ||
        syscall(SYS_capget, &header, caps) < 0) return -1;
    for (int i = 0; i < 2; i++)
        if (caps[i].effective || caps[i].permitted || caps[i].inheritable) return -1;
    return 0;
}

static int sandbox(void)
{
    struct rlimit memory = { 128ul << 20, 128ul << 20 }, files = { 64, 64 }, zero = {0, 0};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_SET_DUMPABLE, 0) < 0 ||
        setrlimit(RLIMIT_AS, &memory) < 0 || setrlimit(RLIMIT_NOFILE, &files) < 0 ||
        setrlimit(RLIMIT_CORE, &zero) < 0 || setrlimit(RLIMIT_NPROC, &zero) < 0 ||
        setrlimit(RLIMIT_FSIZE, &zero) < 0 || setrlimit(RLIMIT_MEMLOCK, &zero) < 0) return -1;
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;
    struct landlock_ruleset_attr rules = { .handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM };
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
    int status = (int)syscall(SYS_landlock_restrict_self, fd, 0);
    close(fd);
    if (status < 0) return -1;
#define ALLOW(call) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##call, 0, 1), \
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#if defined(__x86_64__)
#define ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH AUDIT_ARCH_AARCH64
#else
#error "extdisplay discovery has no sandbox for this architecture"
#endif
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* Avahi asks the pending UDP datagram size before recvmsg. No other
         * ioctl is needed, especially no network configuration operation. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FIONREAD, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        ALLOW(read), ALLOW(write), ALLOW(close), ALLOW(recvfrom), ALLOW(sendto),
        ALLOW(recvmsg), ALLOW(sendmsg), ALLOW(ppoll),
#ifdef SYS_poll
        ALLOW(poll),
#endif
        ALLOW(clock_gettime), ALLOW(gettimeofday), ALLOW(getrandom),
        ALLOW(mmap), ALLOW(mprotect), ALLOW(munmap), ALLOW(mremap), ALLOW(madvise), ALLOW(brk),
        ALLOW(futex), ALLOW(fcntl), ALLOW(getpid), ALLOW(getuid), ALLOW(geteuid),
        ALLOW(getgid), ALLOW(getegid), ALLOW(getsockname), ALLOW(getsockopt), ALLOW(setsockopt),
        ALLOW(rt_sigreturn), ALLOW(rt_sigprocmask), ALLOW(restart_syscall), ALLOW(exit), ALLOW(exit_group),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
    };
    struct sock_fprog program = { .len = sizeof filter / sizeof filter[0], .filter = filter };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

static long number(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return !errno && *text && !*end && value > 0 && value <= INT_MAX ? value : -1;
}

int main(int argc, char **argv)
{
    struct passwd *user;
    struct in6_addr ip;
    unsigned char token[8];
    int type, domain;
    socklen_t size = sizeof type;
    int arg_idx = 1;
    while (arg_idx < argc) {
        if (!strcmp(argv[arg_idx], "--host-id") && arg_idx + 1 < argc) {
            const char *hid = argv[arg_idx + 1];
            if (!valid_host_id(hid, strlen(hid))) return 78;
            snprintf(hostid_txt, sizeof hostid_txt, "hostid=%s", hid);
            has_hostid = 1;
            arg_idx += 2;
        } else if (!strcmp(argv[arg_idx], "--paired-only")) {
            paired_only = 1;
            arg_idx += 1;
        } else {
            break;
        }
    }
    if (argc - arg_idx != 6 || strcmp(argv[arg_idx], "--status-fd") || strcmp(argv[arg_idx + 1], "3") ||
        (interface_index = (int)number(argv[arg_idx + 2])) <= 0 || strlen(argv[arg_idx + 3]) >= IFNAMSIZ ||
        if_nametoindex(argv[arg_idx + 3]) != (unsigned)interface_index ||
        inet_pton(AF_INET6, argv[arg_idx + 4], &ip) != 1 || !IN6_IS_ADDR_LINKLOCAL(&ip) ||
        number(argv[arg_idx + 5]) != getppid() || geteuid() != 0 ||
        getsockopt(STATUS, SOL_SOCKET, SO_TYPE, &type, &size) < 0 || type != SOCK_SEQPACKET ||
        getsockopt(STATUS, SOL_SOCKET, SO_DOMAIN, &domain, &size) < 0 || domain != AF_UNIX ||
        !(user = getpwnam("nobody"))) return 78;
    const char *ifname = argv[arg_idx + 3];
    uid_t uid = user->pw_uid;
    gid_t gid = user->pw_gid;
    pid_t parent = getppid();
    char code[40];
    ssize_t code_len;
    if (syscall(SYS_close_range, 4, ~0U, 0) < 0 ||
        (code_len = recv(STATUS, code, sizeof code - 1, MSG_TRUNC)) <= 0 ||
        getrandom(token, sizeof token, 0) != sizeof token) return 78;
    code[code_len] = 0;
    if (paired_only && !strcmp(code, "PAIRED-ONLY")) {
        has_code = 0;
    } else if (valid_code(code, (size_t)code_len)) {
        snprintf(code_txt, sizeof code_txt, "code=%s", code);
        has_code = 1;
    } else {
        return 78;
    }
    volatile char *wipe = code;
    for (size_t i = 0; i < sizeof code; i++) wipe[i] = 0;
    snprintf(service_name, sizeof service_name, "LOCachy-%02x%02x%02x%02x%02x%02x%02x%02x",
             token[0], token[1], token[2], token[3], token[4], token[5], token[6], token[7]);
    snprintf(host_name, sizeof host_name, "%s.local", service_name);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (drop(uid, gid, parent) < 0) return 78;
    avahi_set_log_function(quiet_log);
    loop = avahi_simple_poll_new();
    if (!loop) return 78;
    const AvahiPoll *api = avahi_simple_poll_get(loop);
    AvahiWatch *owner = api->watch_new(api, STATUS, AVAHI_WATCH_IN | AVAHI_WATCH_HUP | AVAHI_WATCH_ERR,
                                      owner_event, NULL);
    AvahiServerConfig config;
    avahi_server_config_init(&config);
    avahi_free(config.host_name);
    config.host_name = avahi_strdup(service_name);
    config.use_ipv4 = 0;
    config.use_ipv6 = 1;
    config.allow_interfaces = avahi_string_list_new(ifname, NULL);
    config.publish_addresses = config.publish_hinfo = config.publish_workstation = config.publish_domain = 0;
    config.enable_reflector = config.enable_wide_area = 0;
    config.publish_a_on_ipv6 = config.publish_aaaa_on_ipv4 = 0;
    config.n_cache_entries_max = 32;
    config.ratelimit_interval = 1000000;
    config.ratelimit_burst = 50;
    endpoint.proto = AVAHI_PROTO_INET6;
    memcpy(&endpoint.data.ipv6, &ip, sizeof ip);
    int error;
    AvahiServer *server = owner && config.host_name && config.allow_interfaces
        ? avahi_server_new(api, &config, server_event, NULL, &error) : NULL;
    avahi_server_config_free(&config);
    if (!server || failed || sandbox() < 0) return 78;
    sandboxed = 1;
    if (established && send(STATUS, "ready", 5, MSG_NOSIGNAL) != 5) failed = 1;
    while (!failed && !stopping) if (avahi_simple_poll_iterate(loop, -1) < 0) failed = 1;
    api->watch_free(owner);
    if (group) {
        avahi_s_entry_group_free(group);
        /* Cleanup can queue more than one immediate timer before the goodbye.
         * Run until a fixed deadline, rather than counting dispatches. */
        if (!failed) {
            struct timeval deadline;
            avahi_elapse_time(&deadline, 200, 0);
            AvahiTimeout *timer = api->timeout_new(api, &deadline, goodbye_done, NULL);
            if (timer) {
                avahi_simple_poll_loop(loop);
                api->timeout_free(timer);
            }
        }
    }
    avahi_server_free(server); /* withdraw any remaining records */
    avahi_simple_poll_free(loop);
    return failed ? 78 : 0;
}
