/* extdisplay-broker: the privileged half of the extended display (tools/EXTDISPLAY.md).
 *
 * extdisplay starts it as "extdisplay-broker --control-fd N" with one end of a
 * SOCK_SEQPACKET pair and nothing else. On "pair NAME" it leases that
 * Thunderbolt/USB4 network interface: it records the lease in a root-only
 * file under /run, moves the interface out of locachy-net (where NetworkManager
 * has the network) into a fresh network namespace with only its IPv6
 * link-local address, and forks the pump (extdisplay-broker-pump.c), which listens
 * there, completes the TLS 1.2 PSK handshake unprivileged and moves bytes to
 * a plaintext AF_UNIX stream. That stream's other end goes to extdisplay in the
 * "paired" message. On cancel, release, carrier loss, timeout or control EOF
 * the interface goes back to locachy-net if it is still the leased device;
 * NetworkManager then applies its configured policy. The broker never drives
 * NetworkManager.
 *
 * Nothing but the broker process holds the session namespace: no bind mount,
 * no name, and the pump dies with the broker. If the broker dies without a
 * clean end, the namespace collapses; the kernel then moves a hardware device
 * (thunderbolt-net has no link ops) to the initial namespace, and
 * link/90-locachy-network.rules runs network-client, which moves it back into
 * locachy-net. Virtual devices are destroyed with the namespace instead. The
 * record stays until the device is back in locachy-net. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>

#include <openssl/crypto.h>

#include "extdisplay-broker.h"

enum attempt_state { IDLE, ATTEMPT, SESSION };

static int control = -1, home_ns = -1, nl_home = -1, lock_fd = -1, signals = -1, log_fd = 2;
static uid_t pump_uid;
static gid_t pump_gid;
static char boot_id[64];

static struct lease {
    enum attempt_state state;
    char name[IFNAMSIZ];
    int ifindex;
    struct identity id;
    int recorded, moved;
    int sess_ns, nl_sess, nl_mon, sess_ifindex;
    struct in6_addr linklocal;
    pid_t pump;
    int status, front;
    long long deadline;
} L;

static void lease_clear(void)
{
    memset(&L, 0, sizeof L);
    L.state = IDLE;
    L.sess_ns = L.nl_sess = L.nl_mon = L.status = L.front = -1;
    L.pump = -1;
}

void broker_log(const char *format, ...)
{
    char line[600];
    va_list args;
    int n = snprintf(line, sizeof line, log_fd == 2 ? "extdisplay-broker: " : "<6>extdisplay-broker: ");
    va_start(args, format);
    vsnprintf(line + n, sizeof line - (size_t)n - 1, format, args);
    va_end(args);
    strcat(line, "\n");
    (void)!write(log_fd, line, strlen(line));
}

static void close_fd(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static int send_line(const char *text)
{
    size_t n = strlen(text);
    return send(control, text, n, MSG_NOSIGNAL) == (ssize_t)n ? 0 : -1;
}

static int send_fd_line(const char *text, int fd)
{
    struct iovec iov = { (void *)text, strlen(text) };
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.space, .msg_controllen = sizeof u.space };
    memset(&u, 0, sizeof u);
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(control, &m, MSG_NOSIGNAL) == (ssize_t)iov.iov_len ? 0 : -1;
}

static void sleep_ms(int ms)
{
    poll(NULL, 0, ms);
}

/* The lease record, one root-only file per device under /run (gone at boot):
 * written before the device leaves locachy-net, removed once it is back there
 * or never left. A record that outlives its broker means the lease did not
 * end cleanly; that device is not leased again until it is back in
 * locachy-net. Nothing here drives NetworkManager. */
struct record {
    char boot[64];
    char name[IFNAMSIZ];
    int ifindex;
    struct identity id;
};

/* The record file of the device with this address, colons dropped. */
static int record_path(const char *address, char *out, size_t size)
{
    int k = snprintf(out, size, "%s/lease-", BROKER_STATE_DIR);
    size_t n;
    if (k <= 0 || (size_t)k >= size) return -1;
    n = (size_t)k;
    for (const char *p = address; *p; p++) {
        if (*p == ':') continue;
        if (n + 2 >= size) return -1;
        out[n++] = *p;
    }
    out[n] = 0;
    return 0;
}

static void sync_state_dir(void)
{
    int dir = open(BROKER_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
}

static int record_write(void)
{
    char path[PATH_MAX], temp[PATH_MAX + 8], text[1024];
    int n = snprintf(text, sizeof text, "extdisplay-broker-lease 1\nboot %s\nname %s\nifindex %d\naddress %s\ndevice %s\n",
                     boot_id, L.name, L.ifindex, L.id.address, L.id.device);
    if (n <= 0 || (size_t)n >= sizeof text || record_path(L.id.address, path, sizeof path) < 0) return -1;
    snprintf(temp, sizeof temp, "%s.new", path);
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = fchmod(fd, 0600) == 0 && write(fd, text, (size_t)n) == n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(temp, path) < 0) {
        unlink(temp);
        return -1;
    }
    sync_state_dir();
    return 0;
}

static void record_remove(const char *address)
{
    char path[PATH_MAX];
    if (record_path(address, path, sizeof path) == 0 && unlink(path) == 0) sync_state_dir();
}

/* 1 when the file is a complete record. */
static int record_read(const char *path, struct record *r)
{
    char line[600], key[32], value[520];
    int fields = 0, version = 0;
    FILE *f = fopen(path, "re");
    memset(r, 0, sizeof *r);
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t n;
        if (sscanf(line, "%31s %519s", key, value) != 2) continue;
        n = strlen(value) + 1;
        if (!strcmp(key, "extdisplay-broker-lease")) {
            version = atoi(value);
        } else if (!strcmp(key, "boot") && n <= sizeof r->boot) {
            memcpy(r->boot, value, n);
            fields |= 1;
        } else if (!strcmp(key, "name") && valid_ifname(value)) {
            memcpy(r->name, value, n);
            fields |= 2;
        } else if (!strcmp(key, "ifindex")) {
            r->ifindex = atoi(value);
            fields |= 4;
        } else if (!strcmp(key, "address") && n <= sizeof r->id.address) {
            memcpy(r->id.address, value, n);
            fields |= 8;
        } else if (!strcmp(key, "device") && n <= sizeof r->id.device) {
            memcpy(r->id.device, value, n);
            fields |= 16;
        }
    }
    fclose(f);
    return version == 1 && fields == 31;
}

/* The device in the home namespace that has this identity, whatever its
 * name now; never one that merely took the recorded name. */
static int find_identity(const struct identity *id, int hint_ifindex, char *name)
{
    struct link_info info, links[64];
    struct identity seen;
    if (hint_ifindex > 0 && nl_link_get(nl_home, hint_ifindex, &info) == 0 &&
        identity_of(nl_home, info.name, info.ifindex, &seen) == 0 && identity_equal(&seen, id)) {
        strcpy(name, info.name);
        return info.ifindex;
    }
    int n = nl_link_list(nl_home, links, 64);
    for (int i = 0; i < n; i++) {
        if (links[i].type == ARPHRD_LOOPBACK) continue;
        if (identity_of(nl_home, links[i].name, links[i].ifindex, &seen) == 0 && identity_equal(&seen, id)) {
            strcpy(name, links[i].name);
            return links[i].ifindex;
        }
    }
    return -1;
}

/* Records of earlier leases, at start, scan and pair: one of an earlier boot
 * is dropped, and so is one whose device is back in locachy-net; the others
 * stay and are reported. Leftover temporary files are dropped. */
static void stale_check(void)
{
    struct dirent *entry;
    char path[PATH_MAX], name[IFNAMSIZ];
    DIR *dir = opendir(BROKER_STATE_DIR);
    if (!dir) return;
    while ((entry = readdir(dir))) {
        struct record r;
        size_t length = strlen(entry->d_name);
        if (strncmp(entry->d_name, "lease-", 6) != 0) continue;
        snprintf(path, sizeof path, "%s/%s", BROKER_STATE_DIR, entry->d_name);
        if (length > 4 && !strcmp(entry->d_name + length - 4, ".new")) {
            unlink(path);
        } else if (!record_read(path, &r)) {
            broker_log("removing an unreadable lease record");
            unlink(path);
        } else if (strcmp(r.boot, boot_id) != 0) {
            unlink(path);
        } else if (find_identity(&r.id, r.ifindex, name) >= 0) {
            broker_log("lease of %s did not end cleanly; its device is back in locachy-net", r.name);
            unlink(path);
        } else {
            broker_log("lease of %s did not end cleanly; its device is not back in locachy-net yet", r.name);
        }
    }
    closedir(dir);
    sync_state_dir();
}

/* 1 when an unfinished lease of this device is on record. */
static int stale_record_for(const struct identity *id)
{
    char path[PATH_MAX];
    return record_path(id->address, path, sizeof path) == 0 && access(path, F_OK) == 0;
}

/* The device ifindex in the namespace of nl still has the leased address.
 * Netlink only: sysfs shows the namespace that mounted it, not this one. */
static int same_device(int nl, int ifindex, const struct identity *id)
{
    struct link_info info;
    char text[3 * HWADDR_MAX + 1] = "";
    size_t used = 0;
    const unsigned char *a;
    int length, any = 0;
    if (nl_link_get(nl, ifindex, &info) < 0) return 0;
    a = info.perm;
    length = info.perm_len;
    for (int k = 0; k < length; k++) any |= a[k];
    if (!any) {
        a = info.address;
        length = info.address_len;
    }
    for (int k = 0; k < length; k++) used += (size_t)sprintf(text + used, k ? ":%02x" : "%02x", a[k]);
    return length > 0 && strcmp(text, id->address) == 0;
}

static int write_sysctl(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ok = write(fd, value, strlen(value)) == (ssize_t)strlen(value);
    close(fd);
    return ok ? 0 : -1;
}

/* A fresh namespace held only by descriptors of this process. Its defaults
 * apply to devices created there later; the leased device gets its own
 * settings after the move (device_settings), since it keeps the ones it had. */
static int session_namespace(void)
{
    static const char *const settings[][2] = {
        { "/proc/sys/net/ipv4/ip_forward", "0" },
        { "/proc/sys/net/ipv4/conf/all/accept_redirects", "0" },
        { "/proc/sys/net/ipv4/conf/default/accept_redirects", "0" },
        { "/proc/sys/net/ipv6/conf/all/forwarding", "0" },
        { "/proc/sys/net/ipv6/conf/default/forwarding", "0" },
        { "/proc/sys/net/ipv6/conf/all/accept_ra", "0" },
        { "/proc/sys/net/ipv6/conf/default/accept_ra", "0" },
        { "/proc/sys/net/ipv6/conf/all/autoconf", "0" },
        { "/proc/sys/net/ipv6/conf/default/autoconf", "0" },
        { "/proc/sys/net/ipv6/conf/all/accept_redirects", "0" },
        { "/proc/sys/net/ipv6/conf/default/accept_redirects", "0" },
        { "/proc/sys/net/ipv6/conf/default/router_solicitations", "0" },
        { "/proc/sys/net/ipv6/conf/default/use_tempaddr", "0" },
    };
    int ok = 1;
    if (unshare(CLONE_NEWNET) < 0) return -1;
    L.sess_ns = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
    L.nl_sess = nl_open(0);
    L.nl_mon = nl_open(RTMGRP_LINK);
    if (L.sess_ns < 0 || L.nl_sess < 0 || L.nl_mon < 0) ok = 0;
    for (size_t i = 0; ok && i < sizeof settings / sizeof settings[0]; i++) {
        if (write_sysctl(settings[i][0], settings[i][1]) < 0) {
            broker_log("cannot set %s", settings[i][0]);
            ok = 0;
        }
    }
    if (setns(home_ns, CLONE_NEWNET) < 0) {
        /* Stuck in the session namespace: dying returns the device. */
        broker_log("cannot return to the home namespace");
        _exit(1);
    }
    return ok ? 0 : -1;
}

/* Back to the namespace it came from, by the original name or, when another
 * device took that name meanwhile, by its pattern (as network-client does).
 * NetworkManager gets it back only if it is still the recorded device. */
static void lease_release(void)
{
    int returned = 0;
    if (L.pump > 0) {
        /* A running session first gets to send close_notify and close its
         * TCP connection (the pump watches the status socket); 500 ms. */
        if (L.state == SESSION && L.status >= 0 && shutdown(L.status, SHUT_WR) == 0) {
            for (long long deadline = extdisplay_broker_now_ms() + 500;;) {
                char discard[256];
                long long left = deadline - extdisplay_broker_now_ms();
                struct pollfd p = { L.status, POLLIN, 0 };
                if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
                if (recv(L.status, discard, sizeof discard, MSG_DONTWAIT) <= 0) break; /* pump gone */
            }
        }
        kill(L.pump, SIGKILL);
        while (waitpid(L.pump, NULL, 0) < 0 && errno == EINTR) {}
    }
    close_fd(&L.status);
    close_fd(&L.front);
    close_fd(&L.nl_mon);
    if (L.moved && L.nl_sess >= 0) {
        if (!same_device(L.nl_sess, L.sess_ifindex, &L.id)) {
            /* Never move a device that is not the one leased. */
            broker_log("not returning %s: it is not the leased device any more", L.name);
        } else {
            int e = nl_link_move(L.nl_sess, L.sess_ifindex, home_ns, NULL);
            if (e == -EEXIST) {
                /* A device took the name meanwhile: the pattern gives a free one. */
                char pattern[IFNAMSIZ];
                size_t n = strlen(L.name);
                while (n > 0 && L.name[n - 1] >= '0' && L.name[n - 1] <= '9') n--;
                if (n > IFNAMSIZ - 3) n = IFNAMSIZ - 3;
                snprintf(pattern, sizeof pattern, "%.*s%%d", (int)n, L.name);
                e = nl_link_move(L.nl_sess, L.sess_ifindex, home_ns, pattern);
            }
            if (e == 0) {
                returned = 1;
            } else if (e == -ENODEV) {
                broker_log("%s vanished during the lease", L.name);
                returned = 1; /* nothing is left to return */
            } else {
                broker_log("cannot return %s (%s); it stays in its session namespace", L.name, strerror(-e));
            }
        }
    }
    close_fd(&L.nl_sess);
    close_fd(&L.sess_ns); /* the last reference: the session namespace ends */
    /* The record goes when the device is back in locachy-net, or never left.
     * Otherwise it stays, and the device cannot be leased again until it is. */
    if (L.recorded && (returned || !L.moved)) record_remove(L.id.address);
    lease_clear();
}

static void end_attempt(const char *reason)
{
    char line[64];
    lease_release();
    snprintf(line, sizeof line, "ended %s", reason);
    send_line(line);
}

static int random_bytes(unsigned char *out, size_t length)
{
    while (length > 0) {
        ssize_t n = getrandom(out, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        out += n;
        length -= (size_t)n;
    }
    return 0;
}

static void pump_report(const char *line)
{
#ifdef EXTDISPLAY_BROKER_TEST_PROBES
    if (!strncmp(line, "probe ", 6)) {
        broker_log("pump %s", line);
        return;
    }
#endif
    if (!strncmp(line, "error ", 6)) broker_log("pump: %s", line + 6);
}

/* Fresh key, plaintext pair (made here, in the home namespace, so the end
 * extdisplay keeps never references the session namespace), the pump; the code
 * is shown only once the listener exists. */
static const char *start_attempt(void)
{
    struct pump_args a;
    int plain[2] = { -1, -1 }, status[2] = { -1, -1 };
    char code[EXTDISPLAY_BROKER_CODE_TEXT], line[EXTDISPLAY_BROKER_LINE_MAX + 1], address[INET6_ADDRSTRLEN];
    const char *error = NULL;
    memset(&a, 0, sizeof a);
    if (random_bytes(a.key.key, sizeof a.key.key) < 0) return "no random bytes";
    a.key.set = 1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, plain) < 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, status) < 0) {
        error = "cannot create the stream";
        goto out;
    }
    a.netns = L.sess_ns;
    a.ifindex = L.sess_ifindex;
    a.linklocal = L.linklocal;
    a.plain = plain[1];
    a.status = status[1];
    a.uid = pump_uid;
    a.gid = pump_gid;
    a.parent = getpid();
    a.deadline_ms = extdisplay_broker_now_ms() + EXTDISPLAY_BROKER_ATTEMPT_SECONDS * 1000LL;
    L.pump = fork();
    if (L.pump == 0) pump_main(&a);
    close_fd(&plain[1]);
    close_fd(&status[1]);
    L.front = plain[0];
    L.status = status[0];
    plain[0] = status[0] = -1;
    L.deadline = a.deadline_ms;
    if (L.pump < 0) {
        error = "cannot start the pump";
        goto out;
    }
    for (long long deadline = extdisplay_broker_now_ms() + 5000;;) {
        char reply[256];
        long long left = deadline - extdisplay_broker_now_ms();
        struct pollfd p = { L.status, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) {
            error = "the pump did not start";
            goto out;
        }
        ssize_t n = recv(L.status, reply, sizeof reply - 1, 0);
        if (n <= 0) {
            error = "the pump did not start";
            goto out;
        }
        reply[n] = 0;
        if (!strcmp(reply, "listening")) break;
        pump_report(reply);
        if (!strncmp(reply, "error ", 6)) {
            error = "the pump could not listen or sandbox itself";
            goto out;
        }
    }
    if (extdisplay_broker_code_encode(a.key.key, code, sizeof code) < 0 ||
        !inet_ntop(AF_INET6, &L.linklocal, address, sizeof address)) {
        error = "cannot format the pairing";
        goto out;
    }
    snprintf(line, sizeof line, "lease %s %s %d", L.name, address, EXTDISPLAY_BROKER_PORT);
    send_line(line);
    snprintf(line, sizeof line, "code %s", code);
    send_line(line);
    L.state = ATTEMPT;
out:
    OPENSSL_cleanse(&a, sizeof a);
    OPENSSL_cleanse(code, sizeof code);
    OPENSSL_cleanse(line, sizeof line);
    close_fd(&plain[0]);
    close_fd(&plain[1]);
    close_fd(&status[0]);
    close_fd(&status[1]);
    return error;
}

/* The leased device's own IPv6 settings, written inside the session
 * namespace after the move: no router advertisement may add an address or a
 * route, and nothing forwards. Back in the home namespace afterwards. */
static int device_settings(const char *name)
{
    static const char *const keys[] = { "accept_ra", "autoconf", "forwarding", "accept_redirects" };
    char path[PATH_MAX];
    int ok = 1;
    if (!valid_ifname(name) || setns(L.sess_ns, CLONE_NEWNET) < 0) return -1;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        snprintf(path, sizeof path, "/proc/sys/net/ipv6/conf/%s/%s", name, keys[i]);
        if (write_sysctl(path, "0") < 0) ok = 0;
    }
    if (setns(home_ns, CLONE_NEWNET) < 0) {
        broker_log("cannot return to the home namespace");
        _exit(1);
    }
    return ok ? 0 : -1;
}

static const char *lease_start(const char *name)
{
    char driver[64];
    struct link_info links[8];
    int other = 0;
    if (L.state != IDLE) return "busy";
    if (!valid_ifname(name) || sysfs_driver(name, driver, sizeof driver) < 0 || strcmp(driver, BROKER_DRIVER) != 0)
        return "not a Thunderbolt/USB4 network interface";
    memcpy(L.name, name, strlen(name) + 1); /* valid_ifname: below IFNAMSIZ */
    if ((L.ifindex = sysfs_ifindex(name)) < 0 || identity_of(nl_home, name, L.ifindex, &L.id) < 0)
        return "cannot identify the interface";
    stale_check();
    if (stale_record_for(&L.id)) {
        return "the previous lease of this device did not end cleanly; it is not back in locachy-net yet";
    }
    if (record_write() < 0) return "cannot record the lease";
    L.recorded = 1;
    if (session_namespace() < 0) return "cannot create the session namespace";
    if (nl_link_move(nl_home, L.ifindex, L.sess_ns, NULL) < 0) return "cannot move the interface";
    L.moved = 1;
    /* The only device there besides loopback, and still the same one. */
    int n = nl_link_list(L.nl_sess, links, 8);
    for (int i = 0; i < n; i++)
        if (links[i].type != ARPHRD_LOOPBACK) L.sess_ifindex = links[i].ifindex;
    if (!L.sess_ifindex || !same_device(L.nl_sess, L.sess_ifindex, &L.id)) return "the interface changed while moving";
    if (device_settings(L.name) < 0) return "cannot set the interface's IPv6 settings";
    if (nl_link_up(L.nl_sess, L.sess_ifindex) < 0) return "cannot bring the interface up";
    int found = 0;
    for (long long deadline = extdisplay_broker_now_ms() + 10000; extdisplay_broker_now_ms() < deadline; sleep_ms(100)) {
        if ((found = nl_linklocal(L.nl_sess, L.sess_ifindex, &L.linklocal, &other)) != 0) break;
    }
    if (found < 0) return "cannot read the interface addresses";
    if (!found) return "no link-local address (no carrier?)";
    if (other || nl_default_route(L.nl_sess) != 0) return "the session namespace is not link-local only";
    struct link_info info;
    if (nl_link_get(L.nl_sess, L.sess_ifindex, &info) < 0 || !(info.flags & IFF_LOWER_UP)) return "no carrier";
    (void)nl_carrier_event(L.nl_mon, L.sess_ifindex); /* setup's own events */
    return start_attempt();
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan(void)
{
    char names[EXTDISPLAY_BROKER_SCAN_MAX][IFNAMSIZ], *sorted[EXTDISPLAY_BROKER_SCAN_MAX], driver[64], line[128];
    int count = 0;
    struct dirent *entry;
    DIR *dir = opendir("/sys/class/net");
    stale_check();
    while (dir && (entry = readdir(dir)) && count < EXTDISPLAY_BROKER_SCAN_MAX) {
        if (entry->d_name[0] == '.' || !valid_ifname(entry->d_name)) continue;
        if (sysfs_driver(entry->d_name, driver, sizeof driver) < 0 || strcmp(driver, BROKER_DRIVER) != 0) continue;
        memcpy(names[count], entry->d_name, strlen(entry->d_name) + 1); /* valid_ifname */
        sorted[count] = names[count];
        count++;
    }
    if (dir) closedir(dir);
    qsort(sorted, (size_t)count, sizeof sorted[0], compare_names);
    for (int i = 0; i < count; i++) {
        snprintf(line, sizeof line, "iface %s %s %s", sorted[i], BROKER_DRIVER, sysfs_carrier(sorted[i]) ? "up" : "down");
        send_line(line);
    }
    send_line("end");
}

/* 0 go on, -1 control is gone. */
static int handle_control(void)
{
    char line[EXTDISPLAY_BROKER_LINE_MAX + 2];
    ssize_t n = recv(control, line, sizeof line - 1, MSG_TRUNC);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    if (n <= 0) return -1;
    if ((size_t)n > EXTDISPLAY_BROKER_LINE_MAX) {
        send_line("error line too long");
        return 0;
    }
    line[n] = 0;
    if (n > 0 && line[n - 1] == '\n') line[--n] = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (line[i] < 0x20 || line[i] > 0x7e) {
            send_line("error not a printable line");
            return 0;
        }
    }
    if (!strcmp(line, "scan")) {
        if (L.state != IDLE) send_line("error busy");
        else scan();
    } else if (!strncmp(line, "pair ", 5)) {
        const char *error = lease_start(line + 5);
        if (error) {
            char reply[EXTDISPLAY_BROKER_LINE_MAX + 1];
            if (strcmp(error, "busy") != 0) lease_release();
            snprintf(reply, sizeof reply, "error %s", error);
            send_line(reply);
        }
    } else if (!strcmp(line, "cancel")) {
        /* Exactly one "ended" per attempt: a cancel that crosses an end the
         * broker already sent (or an error answer to pair) gets nothing. */
        if (L.state != IDLE) end_attempt("cancelled");
    } else if (!strcmp(line, "release")) {
        end_attempt("released");
        exit(0);
    } else {
        send_line("error unknown command");
    }
    return 0;
}

static void handle_status(void)
{
    char line[256];
    ssize_t n = recv(L.status, line, sizeof line - 1, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (n <= 0) {
        /* The pump is gone without saying why. */
        end_attempt("error");
        return;
    }
    line[n] = 0;
    if (!strcmp(line, "paired") && L.state == ATTEMPT) {
        if (send_fd_line("paired", L.front) < 0) {
            end_attempt("error");
            return;
        }
        close_fd(&L.front);
        L.state = SESSION;
    } else if (!strcmp(line, "handshake-failed") && L.state == ATTEMPT) {
        end_attempt("handshake-failed");
    } else if (!strcmp(line, "timeout") && L.state == ATTEMPT) {
        end_attempt("timeout");
    } else if (!strncmp(line, "closed", 6)) {
        /* The peer closed, or the stream or TLS failed: extdisplay sees EOF on its
         * stream too; the attempt's one "ended" says so and the device goes
         * back now. The fixed reasons have no separate "closed". */
        end_attempt("error");
    } else {
        pump_report(line);
    }
}

static void __attribute__((noreturn)) startup_fail(const char *text)
{
    char line[EXTDISPLAY_BROKER_LINE_MAX + 1];
    broker_log("%s", text);
    snprintf(line, sizeof line, "error %s", text);
    send_line(line);
    exit(1);
}

static int state_dir(void)
{
    char parent[sizeof BROKER_STATE_DIR];
    struct stat st;
    strcpy(parent, BROKER_STATE_DIR);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir(parent, 0755) < 0 && errno != EEXIST) return -1;
    }
    if (mkdir(BROKER_STATE_DIR, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(BROKER_STATE_DIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
        return -1;
    lock_fd = open(BROKER_STATE_DIR "/lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    return lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0 ? 0 : -1;
}

/* The broker lives in the namespace where NetworkManager has the network,
 * with a sysfs of that namespace in a mount namespace of its own (as ip
 * netns exec does), so /sys/class/net lists the candidates there. */
static int enter_home(void)
{
    /* No fallback: the host's namespace must never be taken for the network's. */
    int ns = open(BROKER_HOME_NETNS, O_RDONLY | O_CLOEXEC);
    if (ns < 0) return -1;
    if (unshare(CLONE_NEWNS) < 0 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0 ||
        setns(ns, CLONE_NEWNET) < 0 ||
        mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL) < 0) {
        close(ns);
        return -1;
    }
    home_ns = ns;
    return 0;
}

static int take_control(const char *text)
{
    char *end;
    int type = 0, domain = 0;
    socklen_t size = sizeof type;
    long fd = strtol(text, &end, 10);
    if (*end || end == text || fd < 0 || fd > INT_MAX) return -1;
    if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &size) < 0 || type != SOCK_SEQPACKET) return -1;
    size = sizeof domain;
    if (getsockopt((int)fd, SOL_SOCKET, SO_DOMAIN, &domain, &size) < 0 || domain != AF_UNIX) return -1;
    if (fd <= 2) {
        int moved = fcntl((int)fd, F_DUPFD_CLOEXEC, 3);
        if (moved < 0) return -1;
        close((int)fd);
        fd = moved;
    }
    return (int)fd;
}

int main(int argc, char **argv)
{
    sigset_t set;
    struct passwd *pw;
    char path_boot[] = "/proc/sys/kernel/random/boot_id";
    if (argc != 3 || strcmp(argv[1], "--control-fd") != 0 || (control = take_control(argv[2])) < 0) {
        fprintf(stderr, "usage: extdisplay-broker --control-fd N (a SOCK_SEQPACKET socket from extdisplay)\n");
        return 2;
    }
    /* Only the control socket and the standard descriptors stay. */
    for (int i = 0; i <= 2; i++) {
        if (fcntl(i, F_GETFD) < 0 && open("/dev/null", O_RDWR) != i) return 2;
    }
    /* extdisplay gives /dev/null as stderr: log to the kernel log, as
     * network-client does. */
    struct stat err;
    if (fstat(2, &err) == 0 && S_ISCHR(err.st_mode) && major(err.st_rdev) == 1 && minor(err.st_rdev) == 3)
        log_fd = -1;
    syscall(SYS_close_range, 3, (unsigned)control - 1, 0);
    syscall(SYS_close_range, (unsigned)control + 1, ~0U, 0);
    fcntl(control, F_SETFD, FD_CLOEXEC);
    if (log_fd < 0 && (log_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC)) < 0) log_fd = 2;
    lease_clear();
    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &set, NULL) < 0 || (signals = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK)) < 0)
        startup_fail("cannot handle signals");
    if (geteuid() != 0) startup_fail("extdisplay-broker must run as root");
    if (!(pw = getpwnam(BROKER_PUMP_USER)) || !pw->pw_uid || !pw->pw_gid) startup_fail("no unprivileged user for the pump");
    pump_uid = pw->pw_uid;
    pump_gid = pw->pw_gid;
    FILE *f = fopen(path_boot, "re");
    if (!f || !fgets(boot_id, sizeof boot_id, f)) startup_fail("cannot read the boot id");
    fclose(f);
    boot_id[strcspn(boot_id, "\n")] = 0;
    if (state_dir() < 0) startup_fail("another broker is running, or its state directory is unusable");
    if (enter_home() < 0) startup_fail("cannot enter the network namespace of the network");
    if ((nl_home = nl_open(0)) < 0) startup_fail("no rtnetlink");
    struct extdisplay_broker_key probe = { .set = 0 };
    SSL_CTX *ctx = extdisplay_broker_tls_init() == 0 ? extdisplay_broker_tls_context(&probe) : NULL;
    if (!ctx) startup_fail("OpenSSL cannot provide the cable TLS profile");
    SSL_CTX_free(ctx);
    stale_check();

    for (;;) {
        struct pollfd p[4] = { { control, POLLIN, 0 }, { signals, POLLIN, 0 }, { L.status, POLLIN, 0 },
                               { L.nl_mon, POLLIN, 0 } };
        int timeout = -1;
        if (L.state == ATTEMPT) {
            /* The pump reports its own timeout; this is the backstop. */
            long long left = L.deadline + 2000 - extdisplay_broker_now_ms();
            timeout = left < 0 ? 0 : left > 60000 ? 60000 : (int)left;
        }
        if (poll(p, 4, timeout) < 0 && errno != EINTR) startup_fail("poll failed");
        if (p[1].revents & POLLIN) {
            struct signalfd_siginfo info;
            if (read(signals, &info, sizeof info) == sizeof info) {
                lease_release();
                return 0;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (handle_control() < 0) {
                lease_release();
                return 0;
            }
            continue; /* the lease may have changed */
        }
        if (L.status >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) handle_status();
        if (L.nl_mon >= 0 && (p[3].revents & (POLLIN | POLLERR))) {
            int lost = nl_carrier_event(L.nl_mon, L.sess_ifindex);
            struct link_info info;
            if (lost < 0) lost = nl_link_get(L.nl_sess, L.sess_ifindex, &info) < 0 || !(info.flags & IFF_LOWER_UP);
            if (lost) end_attempt("carrier-lost");
        }
        if (L.state == ATTEMPT && extdisplay_broker_now_ms() > L.deadline + 2000) end_attempt("timeout");
    }
}
