/* kvm-broker-net.c: rtnetlink and sysfs for kvm-broker. Every rtnetlink
 * socket acts on the namespace it was created in; the broker creates one per
 * namespace. sysfs is read through the broker's own mount of it, made in the
 * home namespace (kvm-broker.c). */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>
#ifdef KVM_BROKER_TEST_DRIVER
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#endif

#include "kvm-broker.h"

static unsigned sequence;
static char buffer[65536];

struct request {
    struct nlmsghdr header;
    union {
        struct ifinfomsg link;
        struct ifaddrmsg address;
        struct rtmsg route;
    };
    char attributes[256];
};

static void add_attribute(struct request *r, unsigned short type, const void *data, size_t length)
{
    struct rtattr *a = (struct rtattr *)((char *)r + NLMSG_ALIGN(r->header.nlmsg_len));
    if (NLMSG_ALIGN(r->header.nlmsg_len) + RTA_SPACE(length) > sizeof *r) return;
    a->rta_type = type;
    a->rta_len = (unsigned short)RTA_LENGTH(length);
    memcpy(RTA_DATA(a), data, length);
    r->header.nlmsg_len = NLMSG_ALIGN(r->header.nlmsg_len) + RTA_ALIGN(a->rta_len);
}

int nl_open(unsigned groups)
{
    struct sockaddr_nl local = { .nl_family = AF_NETLINK, .nl_groups = groups };
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int nl, struct request *r)
{
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
    r->header.nlmsg_seq = ++sequence;
    r->header.nlmsg_pid = 0;
    for (;;) {
        ssize_t n = sendto(nl, r, r->header.nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof kernel);
        if (n == (ssize_t)r->header.nlmsg_len) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

static ssize_t receive(int nl)
{
    for (;;) {
        struct pollfd p = { nl, POLLIN, 0 };
        int ready = poll(&p, 1, 3000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t n = recv(nl, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

/* Walks the replies to request number seq. visit returns <0 to stop with an
 * error. Returns 0 at NLMSG_DONE or an ACK, -errno on a netlink error. */
typedef int (*visit_fn)(struct nlmsghdr *h, void *context);

static int replies(int nl, unsigned seq, int dump, visit_fn visit, void *context)
{
    for (;;) {
        ssize_t got = receive(nl);
        if (got <= 0) return got < 0 ? -errno : -EIO;
        int n = (int)got;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buffer; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != seq) continue; /* an event or a late reply */
            if (h->nlmsg_type == NLMSG_DONE) return 0;
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                if (h->nlmsg_len < NLMSG_LENGTH(sizeof *e)) return -EIO;
                return e->error;
            }
            if (visit && visit(h, context) < 0) return -EIO;
            if (!dump) return 0;
        }
    }
}

static void parse_link(struct nlmsghdr *h, struct link_info *info)
{
    struct ifinfomsg *ifi = NLMSG_DATA(h);
    int length = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof *ifi);
    memset(info, 0, sizeof *info);
    info->ifindex = ifi->ifi_index;
    info->flags = ifi->ifi_flags;
    info->type = ifi->ifi_type;
    for (struct rtattr *a = IFLA_RTA(ifi); length > 0 && RTA_OK(a, length); a = RTA_NEXT(a, length)) {
        size_t size = RTA_PAYLOAD(a);
        switch (a->rta_type) {
        case IFLA_IFNAME:
            if (size > 0 && size <= IFNAMSIZ) {
                memcpy(info->name, RTA_DATA(a), size);
                info->name[IFNAMSIZ - 1] = 0;
            }
            break;
        case IFLA_ADDRESS:
            if (size <= HWADDR_MAX) {
                memcpy(info->address, RTA_DATA(a), size);
                info->address_len = (int)size;
            }
            break;
        case IFLA_PERM_ADDRESS:
            if (size <= HWADDR_MAX) {
                memcpy(info->perm, RTA_DATA(a), size);
                info->perm_len = (int)size;
            }
            break;
        default:
            break;
        }
    }
}

static int link_one(struct nlmsghdr *h, void *context)
{
    if (h->nlmsg_type != RTM_NEWLINK || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) return 0;
    parse_link(h, context);
    return 0;
}

int nl_link_get(int nl, int ifindex, struct link_info *info)
{
    struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                                     .nlmsg_type = RTM_GETLINK, .nlmsg_flags = NLM_F_REQUEST } };
    r.link.ifi_family = AF_UNSPEC;
    r.link.ifi_index = ifindex;
    memset(info, 0, sizeof *info);
    if (send_request(nl, &r) < 0) return -1;
    int e = replies(nl, r.header.nlmsg_seq, 0, link_one, info);
    if (e < 0) {
        errno = -e;
        return -1;
    }
    return info->ifindex == ifindex ? 0 : -1;
}

struct link_list {
    struct link_info *links;
    int count, max;
};

static int link_each(struct nlmsghdr *h, void *context)
{
    struct link_list *list = context;
    if (h->nlmsg_type != RTM_NEWLINK || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) return 0;
    if (list->count < list->max) parse_link(h, &list->links[list->count++]);
    return 0;
}

int nl_link_list(int nl, struct link_info *links, int max)
{
    struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                                     .nlmsg_type = RTM_GETLINK, .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP } };
    struct link_list list = { links, 0, max };
    r.link.ifi_family = AF_UNSPEC;
    if (send_request(nl, &r) < 0) return -1;
    return replies(nl, r.header.nlmsg_seq, 1, link_each, &list) < 0 ? -1 : list.count;
}

int nl_link_move(int nl, int ifindex, int netns_fd, const char *pattern)
{
    struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                                     .nlmsg_type = RTM_NEWLINK, .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK } };
    unsigned fd = (unsigned)netns_fd;
    r.link.ifi_family = AF_UNSPEC;
    r.link.ifi_index = ifindex;
    add_attribute(&r, IFLA_NET_NS_FD, &fd, sizeof fd);
    if (pattern) add_attribute(&r, IFLA_IFNAME, pattern, strlen(pattern) + 1);
    if (send_request(nl, &r) < 0) return -errno;
    return replies(nl, r.header.nlmsg_seq, 0, NULL, NULL);
}

int nl_link_up(int nl, int ifindex)
{
    struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                                     .nlmsg_type = RTM_NEWLINK, .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK } };
    r.link.ifi_family = AF_UNSPEC;
    r.link.ifi_index = ifindex;
    r.link.ifi_flags = IFF_UP;
    r.link.ifi_change = IFF_UP;
    if (send_request(nl, &r) < 0) return -1;
    return replies(nl, r.header.nlmsg_seq, 0, NULL, NULL) == 0 ? 0 : -1;
}

struct address_scan {
    int ifindex, found, other;
    struct in6_addr address;
};

static int address_each(struct nlmsghdr *h, void *context)
{
    struct address_scan *scan = context;
    struct ifaddrmsg *ifa = NLMSG_DATA(h);
    int length = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof *ifa);
    unsigned flags;
    const struct in6_addr *local = NULL;
    if (h->nlmsg_type != RTM_NEWADDR || length < 0) return 0;
    if (ifa->ifa_scope == RT_SCOPE_HOST) return 0; /* loopback's own */
    flags = ifa->ifa_flags;
    for (struct rtattr *a = IFA_RTA(ifa); RTA_OK(a, length); a = RTA_NEXT(a, length)) {
        if (a->rta_type == IFA_FLAGS && RTA_PAYLOAD(a) >= sizeof(unsigned)) memcpy(&flags, RTA_DATA(a), sizeof flags);
        if (a->rta_type == IFA_ADDRESS && ifa->ifa_family == AF_INET6 && RTA_PAYLOAD(a) == sizeof(struct in6_addr))
            local = RTA_DATA(a);
    }
    if (ifa->ifa_family != AF_INET6 || !local || (int)ifa->ifa_index != scan->ifindex ||
        !IN6_IS_ADDR_LINKLOCAL(local)) {
        scan->other = 1;
        return 0;
    }
    if (flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED)) return 0;
    if (!scan->found) {
        scan->address = *local;
        scan->found = 1;
    }
    return 0;
}

int nl_linklocal(int nl, int ifindex, struct in6_addr *address, int *other)
{
    struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)),
                                     .nlmsg_type = RTM_GETADDR, .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP } };
    struct address_scan scan = { .ifindex = ifindex };
    r.address.ifa_family = AF_UNSPEC;
    if (send_request(nl, &r) < 0 || replies(nl, r.header.nlmsg_seq, 1, address_each, &scan) < 0) return -1;
    *other = scan.other;
    if (scan.found) *address = scan.address;
    return scan.found;
}

static int route_each(struct nlmsghdr *h, void *context)
{
    struct rtmsg *rt = NLMSG_DATA(h);
    if (h->nlmsg_type == RTM_NEWROUTE && h->nlmsg_len >= NLMSG_LENGTH(sizeof *rt) && rt->rtm_dst_len == 0 &&
        rt->rtm_type == RTN_UNICAST) {
        *(int *)context = 1;
    }
    return 0;
}

int nl_default_route(int nl)
{
    static const unsigned char families[] = { AF_INET, AF_INET6 };
    int found = 0;
    for (size_t i = 0; i < sizeof families; i++) {
        struct request r = { .header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
                                         .nlmsg_type = RTM_GETROUTE, .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP } };
        r.route.rtm_family = families[i];
        if (send_request(nl, &r) < 0 || replies(nl, r.header.nlmsg_seq, 1, route_each, &found) < 0) return -1;
    }
    return found;
}

int nl_carrier_event(int nl, int ifindex)
{
    int lost = 0;
    for (;;) {
        ssize_t got = recv(nl, buffer, sizeof buffer, MSG_DONTWAIT);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return lost;
        if (got <= 0) return -1; /* ENOBUFS: events were lost */
        int n = (int)got;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buffer; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            struct ifinfomsg *ifi = NLMSG_DATA(h);
            if ((h->nlmsg_type != RTM_NEWLINK && h->nlmsg_type != RTM_DELLINK) ||
                h->nlmsg_len < NLMSG_LENGTH(sizeof *ifi) || ifi->ifi_index != ifindex) {
                continue;
            }
            if (h->nlmsg_type == RTM_DELLINK || !(ifi->ifi_flags & IFF_LOWER_UP) || !(ifi->ifi_flags & IFF_UP)) lost = 1;
        }
    }
}

static int read_line(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    if (!fgets(out, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

int valid_ifname(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= IFNAMSIZ || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* The driver bound to the interface's parent device. Virtual devices have
 * none and are never candidates; a test build lets them stand in for
 * hardware through their ethtool driver name (the fixture veth). */
int sysfs_driver(const char *name, char *driver, size_t size)
{
    char path[PATH_MAX], target[PATH_MAX];
    ssize_t n;
    if (!valid_ifname(name)) return -1;
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", name);
    n = readlink(path, target, sizeof target - 1);
    if (n > 0) {
        target[n] = 0;
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strlen(base) >= size) return -1;
        strcpy(driver, base);
        return 0;
    }
#ifdef KVM_BROKER_TEST_DRIVER
    struct ethtool_drvinfo info = { .cmd = ETHTOOL_GDRVINFO };
    struct ifreq request = { 0 };
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
    request.ifr_data = (char *)&info;
    int r = ioctl(fd, SIOCETHTOOL, &request);
    close(fd);
    if (r < 0 || strlen(info.driver) >= size) return -1;
    strcpy(driver, info.driver);
    return 0;
#else
    return -1;
#endif
}

int sysfs_carrier(const char *name)
{
    char path[PATH_MAX], value[16];
    if (!valid_ifname(name)) return 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/carrier", name);
    return read_line(path, value, sizeof value) == 0 && !strcmp(value, "1");
}

int sysfs_ifindex(const char *name)
{
    char path[PATH_MAX], value[32], *end;
    if (!valid_ifname(name)) return -1;
    snprintf(path, sizeof path, "/sys/class/net/%s/ifindex", name);
    if (read_line(path, value, sizeof value) < 0) return -1;
    long v = strtol(value, &end, 10);
    return *end || v <= 0 || v > INT_MAX ? -1 : (int)v;
}

static void format_address(const unsigned char *a, int length, char *out)
{
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < length; i++) used += (size_t)sprintf(out + used, i ? ":%02x" : "%02x", a[i]);
}

int identity_of(int nl, const char *name, int ifindex, struct identity *id)
{
    struct link_info info;
    char path[PATH_MAX], real[PATH_MAX];
    int perm = 0;
    memset(id, 0, sizeof *id);
    if (!valid_ifname(name) || nl_link_get(nl, ifindex, &info) < 0 || strcmp(info.name, name) != 0) return -1;
    for (int i = 0; i < info.perm_len; i++) perm |= info.perm[i];
    if (perm) format_address(info.perm, info.perm_len, id->address);
    else if (info.address_len > 0) format_address(info.address, info.address_len, id->address);
    else return -1;
    snprintf(path, sizeof path, "/sys/class/net/%s/device", name);
    if (!realpath(path, real)) strcpy(id->device, "-");
    else if (strlen(real) < sizeof id->device) strcpy(id->device, real);
    else return -1;
    return 0;
}

int identity_equal(const struct identity *a, const struct identity *b)
{
    return a->address[0] && !strcmp(a->address, b->address) && !strcmp(a->device, b->device);
}
