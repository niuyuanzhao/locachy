/* Internal interfaces of kvm-broker (tools/KVM.md): kvm-broker.c (control,
 * lease lifecycle, NetworkManager, recovery record), kvm-broker-net.c
 * (rtnetlink and sysfs) and kvm-broker-pump.c (the unprivileged TLS pump). */
#ifndef LOCACHY_KVM_BROKER_H
#define LOCACHY_KVM_BROKER_H

#include <net/if.h>
#include <netinet/in.h>
#include <sys/types.h>

#include "kvm-broker-protocol.h"

/* glibc's net/if.h stops at IFF_DYNAMIC; linux/if.h, which has the rest,
 * conflicts with it. The kernel's value (include/uapi/linux/if.h). */
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP (1 << 16)
#endif

/* Compile-time configuration; the tests build with fixtures, there is no
 * runtime override of any of these. */
#ifdef KVM_BROKER_TEST_DRIVER
#define BROKER_DRIVER KVM_BROKER_TEST_DRIVER
#else
#define BROKER_DRIVER "thunderbolt-net"
#endif
/* Where NetworkManager has the network (link/network-client). Required: the
 * broker refuses to start without it, never using the host's namespace. */
#ifdef KVM_BROKER_TEST_HOME_NETNS
#define BROKER_HOME_NETNS KVM_BROKER_TEST_HOME_NETNS
#else
#define BROKER_HOME_NETNS "/run/netns/locachy-net"
#endif
#ifdef KVM_BROKER_TEST_STATE_DIR
#define BROKER_STATE_DIR KVM_BROKER_TEST_STATE_DIR
#else
#define BROKER_STATE_DIR "/run/locachy/kvm-broker"
#endif
#define BROKER_PUMP_USER "nobody"

#define HWADDR_MAX 32

void broker_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* kvm-broker-net.c */
struct link_info {
    int ifindex;
    unsigned flags;     /* IFF_* */
    unsigned short type; /* ARPHRD_* */
    char name[IFNAMSIZ];
    unsigned char address[HWADDR_MAX];
    int address_len;
    unsigned char perm[HWADDR_MAX];
    int perm_len;
};

int nl_open(unsigned groups);
int nl_link_get(int nl, int ifindex, struct link_info *info);
/* Every link; at most max entries. Returns the count or -1. */
int nl_link_list(int nl, struct link_info *links, int max);
/* Into the namespace of netns_fd; pattern (with %d) only when the name is
 * taken there. Returns 0 or -errno. */
int nl_link_move(int nl, int ifindex, int netns_fd, const char *pattern);
int nl_link_up(int nl, int ifindex);
/* 1: a usable fe80::/10 address is on ifindex (copied out); 0: none yet;
 * -1: error. *other is set when any other address exists in the namespace
 * besides loopback's. */
int nl_linklocal(int nl, int ifindex, struct in6_addr *address, int *other);
/* 1: a default route (IPv4 or IPv6) exists, 0: none, -1: error. */
int nl_default_route(int nl);
/* Reads RTM_NEWLINK/RTM_DELLINK events from a socket bound to RTMGRP_LINK:
 * 1 when ifindex lost its carrier or vanished, 0 otherwise, -1 on overrun
 * (the caller re-reads the link). */
int nl_carrier_event(int nl, int ifindex);

/* The identity that names a device whatever its name or namespace: its
 * permanent hardware address (or the current one when it has none) and the
 * sysfs path of its parent device ("-" for a virtual device). */
struct identity {
    char address[3 * HWADDR_MAX];
    char device[512];
};
int sysfs_driver(const char *name, char *driver, size_t size);
int sysfs_carrier(const char *name);
int sysfs_ifindex(const char *name);
int identity_of(int nl, const char *name, int ifindex, struct identity *id);
int identity_equal(const struct identity *a, const struct identity *b);
int valid_ifname(const char *name);

/* kvm-broker-pump.c */
struct pump_args {
    int netns;      /* session namespace */
    int ifindex;    /* the leased device there */
    struct in6_addr linklocal;
    int plain;      /* plaintext stream end */
    int status;     /* SOCK_SEQPACKET to the broker */
    uid_t uid;
    gid_t gid;
    pid_t parent;
    long long deadline_ms;
    struct kvm_broker_key key;
};
/* Runs in the forked child and never returns. */
void pump_main(struct pump_args *args) __attribute__((noreturn));

#endif
