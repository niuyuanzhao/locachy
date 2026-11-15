/* Real Avahi-core peer and production lifecycle wrapper for namespace tests.
 * No system avahi-daemon is contacted. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-discovery.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-core/core.h>
#include <avahi-core/lookup.h>
#include <avahi-core/log.h>

static AvahiSimplePoll *loop;
static int found, removed, invalid;
static void quiet(AvahiLogLevel level, const char *text) { (void)level; (void)text; }

static void resolve(AvahiSServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
                    AvahiResolverEvent event, const char *name, const char *type, const char *domain,
                    const char *host, const AvahiAddress *address, uint16_t port, AvahiStringList *txt,
                    AvahiLookupResultFlags flags, void *userdata)
{
    (void)protocol; (void)type; (void)domain; (void)flags; (void)userdata;
    if (event == AVAHI_RESOLVER_FOUND && address && address->proto == AVAHI_PROTO_INET6) {
        struct in6_addr ip;
        memcpy(&ip, &address->data.ipv6, sizeof ip);
        int count = 0, pv = 0, extended = 0, pair = 0, code = 0;
        for (AvahiStringList *item = txt; item; item = avahi_string_list_get_next(item)) {
            size_t n = avahi_string_list_get_size(item);
            const unsigned char *text = avahi_string_list_get_text(item);
            count++;
            pv |= n == 4 && !memcmp(text, "pv=3", n);
            extended |= n == 10 && !memcmp(text, "extended=1", n);
            pair |= n == 11 && !memcmp(text, "pair=psk128", n);
            code |= n == 37 && !memcmp(text, "code=AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AA", n);
        }
        if (!IN6_IS_ADDR_LINKLOCAL(&ip) || port != 27183 || count != 4 || !pv || !extended || !pair || !code ||
            strncmp(name, "LOCachy-", 8) || strncmp(host, "LOCachy-", 8)) invalid = 1;
        found++;
        printf("FOUND %d %u valid=%d\n", interface, (unsigned)port, !invalid);
        fflush(stdout);
    }
    avahi_s_service_resolver_free(r);
}

static void browse(AvahiSServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
                   AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
                   AvahiLookupResultFlags flags, void *userdata)
{
    (void)b; (void)flags;
    if (event == AVAHI_BROWSER_NEW) {
        if (!avahi_s_service_resolver_new(userdata, interface, protocol, name, type, domain,
                                          AVAHI_PROTO_INET6, AVAHI_LOOKUP_USE_MULTICAST, resolve, NULL)) invalid = 1;
    } else if (event == AVAHI_BROWSER_REMOVE) {
        removed++;
        puts("REMOVED");
        fflush(stdout);
    } else if (event == AVAHI_BROWSER_FAILURE) invalid = 1;
}

static void server_state(AvahiServer *server, AvahiServerState state, void *userdata)
{
    (void)userdata;
    if (state == AVAHI_SERVER_FAILURE) invalid = 1;
    (void)server;
}

static int browser(const char *name, int seconds)
{
    avahi_set_log_function(quiet);
    loop = avahi_simple_poll_new();
    if (!loop) return 1;
    AvahiServerConfig config;
    avahi_server_config_init(&config);
    config.use_ipv4 = 0;
    config.use_ipv6 = 1;
    config.disable_publishing = 1;
    config.enable_reflector = config.enable_wide_area = 0;
    config.allow_interfaces = avahi_string_list_new(name, NULL);
    int error;
    AvahiServer *server = avahi_server_new(avahi_simple_poll_get(loop), &config, server_state, NULL, &error);
    avahi_server_config_free(&config);
    if (!server) return 1;
    AvahiSServiceBrowser *b = avahi_s_service_browser_new(server, if_nametoindex(name), AVAHI_PROTO_INET6,
        "_locachy-ext._tcp", "local", AVAHI_LOOKUP_USE_MULTICAST, browse, server);
    if (!b) return 1;
    puts("BROWSING");
    fflush(stdout);
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end && !invalid) if (avahi_simple_poll_iterate(loop, 100) < 0) invalid = 1;
    avahi_s_service_browser_free(b);
    avahi_server_free(server);
    avahi_simple_poll_free(loop);
    printf("RESULT found=%d removed=%d invalid=%d\n", found, removed, invalid);
    return invalid ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "browse")) return browser(argv[2], atoi(argv[3]));
    if (argc != 5 || strcmp(argv[1], "advertise")) return 2;
    struct in6_addr ip;
    struct extdisplay_discovery d = { .pid = -1, .status = -1 };
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd < 0 || inet_pton(AF_INET6, argv[4], &ip) != 1 ||
        extdisplay_discovery_start(&d, fd, atoi(argv[3]), "rx", &ip, "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AA") < 0) return 1;
    close(fd);
    int ready = 0;
    for (int i = 0; i < 100 && !ready; i++) {
        struct pollfd p = { d.status, POLLIN, 0 };
        if (poll(&p, 1, 100) > 0) ready = extdisplay_discovery_status(&d);
    }
    if (ready != 1) { extdisplay_discovery_stop(&d); return 1; }
    printf("READY %ld\n", (long)d.pid);
    fflush(stdout);
    char byte;
    (void)!read(0, &byte, 1);
    extdisplay_discovery_stop(&d);
    puts("STOPPED");
    return 0;
}
