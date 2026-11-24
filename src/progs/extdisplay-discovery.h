/* Scoped DNS-SD advertisement for one extdisplay pairing attempt.
 *
 * The child is started only after the interface has been moved into the
 * session namespace and has a link-local address. It has no pairing key and
 * never handles the TLS or plaintext stream. */
#ifndef LOCACHY_EXTDISPLAY_DISCOVERY_H
#define LOCACHY_EXTDISPLAY_DISCOVERY_H

#include <netinet/in.h>
#include <sys/types.h>

struct extdisplay_discovery {
    pid_t pid;
    int status; /* nonblocking status socket; -1 when stopped */
};

/* Start an IPv6-only _locachy-ext._tcp advertisement in netns. The service is
 * limited to ifindex, carries the current attempt's Base32 pairing code over
 * the dedicated Thunderbolt/USB4 link, and is removed when stop is called or
 * the child dies. */
int extdisplay_discovery_start(struct extdisplay_discovery *d, int netns, int ifindex,
                               const char *name, const struct in6_addr *address, const char *code);
/* Extended variant that also attaches an optional stable host_id and/or
 * paired-only flag (when paired_only != 0, code is omitted from mDNS TXT). */
int extdisplay_discovery_start_ex(struct extdisplay_discovery *d, int netns, int ifindex,
                                  const char *name, const struct in6_addr *address,
                                  const char *code, const char *host_id, int paired_only);
/* 1: advertisement established, 0: still starting, -1: failed or gone. */
int extdisplay_discovery_status(struct extdisplay_discovery *d);
void extdisplay_discovery_stop(struct extdisplay_discovery *d);

#endif
