/* Unit test, no netlink socket or module loading:
 * cc -D_GNU_SOURCE -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
 *    -Ibase/include tools/test-modules-event.c -Wl,--gc-sections \
 *    -o /tmp/test-modules-event && /tmp/test-modules-event
 */
#include <assert.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct packet {
    const char *data;
    size_t size;
};

static const struct packet *packets;
static size_t npackets, received;
static int receive_error = EAGAIN;
static int nmatched;
static char matched[8][64];

static ssize_t test_recv(int fd, void *buf, size_t size, int flags)
{
    assert(fd == 91 && flags == MSG_DONTWAIT);
    if (received == npackets) {
        errno = receive_error;
        return receive_error ? -1 : 0;
    }
    const struct packet *p = &packets[received++];
    assert(p->size <= size);
    memset(buf, 0xA5, size); /* No accidental terminator beyond the packet. */
    memcpy(buf, p->data, p->size);
    return (ssize_t)p->size;
}

static int test_fnmatch(const char *pattern, const char *alias, int flags)
{
    assert(strcmp(pattern, "test:*") == 0 && flags == 0);
    assert(nmatched < (int)(sizeof matched / sizeof matched[0]));
    assert(strlen(alias) < sizeof matched[0]);
    strcpy(matched[nmatched++], alias);
    return FNM_NOMATCH; /* Record load_for(), but never load a module. */
}

#define recv test_recv
#define fnmatch test_fnmatch
#include "../base/modload.c"
#undef fnmatch
#undef recv

/* Any attempt to reach the real module loader is a test failure. */
void say(const char *fmt, ...)
{
    (void)fmt;
    abort();
}

double uptime(void)
{
    abort();
}

static void check_packets(const struct packet *p, size_t count, int changed, int matches)
{
    static struct alias table[] = { { "test:*", "unused" } };
    aliases = table;
    naliases = 1;
    uevents = 91;
    packets = p;
    npackets = count;
    received = 0;
    nmatched = 0;
    assert(modules_event() == changed);
    assert(received == npackets && nmatched == matches);
    assert(modules_event() == 0); /* Change is per drain, not sticky. */
    assert(nmatched == matches);
}

static void check(const char *data, size_t size, int changed, int matches)
{
    const struct packet p = { data, size };
    check_packets(&p, 1, changed, matches);
}

#define CHECK(data, changed, matches) check(data, sizeof(data) - 1, changed, matches)

int main(void)
{
    CHECK("add@/drm\0ACTION=add\0SUBSYSTEM=drm\0MODALIAS=test:drm\0", 1, 1);
    assert(strcmp(matched[0], "test:drm") == 0);
    CHECK("change@/drm\0SUBSYSTEM=drm\0ACTION=change\0MODALIAS=test:skip\0", 1, 0);
    CHECK("remove@/drm\0ACTION=remove\0SUBSYSTEM=drm\0MODALIAS=test:skip\0", 1, 0);
    CHECK("change@/drm\0HOTPLUG=1\0ACTION=bind\0SUBSYSTEM=drm\0", 1, 0);
    CHECK("change@/drm\0SUBSYSTEM=drm\0HOTPLUG=1\0", 1, 0);
    CHECK("change@/drm\0SUBSYSTEM=drm\0ACTION=bind\0HOTPLUG=0\0", 0, 0);
    CHECK("change@/drm\0SUBSYSTEM=drm\0ACTION=addition\0HOTPLUG=10\0", 0, 0);
    CHECK("change@/drm\0SUBSYSTEM=drm-extra\0ACTION=change\0HOTPLUG=1\0", 0, 0);
    CHECK("change@/drm\0ACTION=change\0HOTPLUG=1\0", 0, 0);
    CHECK("add@/pci\0SUBSYSTEM=pci\0ACTION=add\0HOTPLUG=1\0MODALIAS=test:pci\0", 0, 1);
    CHECK("change@/pci\0SUBSYSTEM=pci\0ACTION=add\0MODALIAS=test:skip\0", 0, 0);
    /* Loading still follows the kernel add@ header, independently of ACTION. */
    CHECK("add@/pci\0ACTION=change\0MODALIAS=test:first\0MODALIAS=test:second\0", 0, 2);
    assert(strcmp(matched[0], "test:first") == 0);
    assert(strcmp(matched[1], "test:second") == 0);
    CHECK("add@/drm\0ACTION=add\0SUBSYSTEM=drm\0\0", 1, 0);
    CHECK("add@/drm\0ACTION=add\0", 0, 0);
    CHECK("change@/drm\0SUBSYSTEM=drm\0", 0, 0);
    /* A replay (udevadm trigger): its module is loaded, but it changes no display. */
    CHECK("add@/drm/card1\0ACTION=add\0SUBSYSTEM=drm\0SYNTH_UUID=0\0MODALIAS=test:replay\0", 0, 1);
    CHECK("change@/drm/card1-DP-3\0ACTION=change\0SUBSYSTEM=drm\0HOTPLUG=1\0"
          "SYNTH_UUID=6f1b2c3d-1111-2222-3333-444455556666\0", 0, 0);
    /* The kernel's notice of a lease created or destroyed. */
    CHECK("change@/devices/pci0000:00/0000:00:02.0/drm/card1\0ACTION=change\0"
          "DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card1\0SUBSYSTEM=drm\0LEASE=1\0"
          "DEVNAME=dri/card1\0DEVTYPE=drm_minor\0", 0, 0);

    /* Processed udev events only wake init's input refresh. Their properties
     * must not replay driver loads or produce another DRM handover. */
    CHECK("libudev\0binary\0ACTION=add\0SUBSYSTEM=drm\0HOTPLUG=1\0MODALIAS=test:udev\0", 0, 0);
    CHECK("libudev\0", 0, 0);

    /* Ignore unterminated headers/fields, without reading outside recv(). */
    CHECK("", 0, 0);
    CHECK("a", 0, 0);
    CHECK("\0", 0, 0);
    CHECK("add@/drm", 0, 0);
    CHECK("add@/drm\0", 0, 0);
    CHECK("add@/drm\0SUBSYSTEM=drm\0ACTION=add", 0, 0);
    CHECK("add@/drm\0ACTION=add\0SUBSYSTEM=drm", 0, 0);
    CHECK("add@/drm\0SUBSYSTEM=drm\0ACTION=add\0MODALIAS=test:partial", 1, 0);
    CHECK("add@/pci\0MODALIAS=test:ok\0MODALIAS=test:partial", 0, 1);
    assert(strcmp(matched[0], "test:ok") == 0);

    char full[8192];
    memset(full, 'x', sizeof full);
    check(full, sizeof full, 0, 0);
    memcpy(full, "add@", 4);
    static const char tail[] = "\0SUBSYSTEM=drm\0ACTION=add\0MODALIAS=test:full\0";
    memcpy(full + sizeof full - (sizeof tail - 1), tail, sizeof tail - 1);
    check(full, sizeof full, 1, 1);
    assert(strcmp(matched[0], "test:full") == 0);
    full[sizeof full - 1] = 'x';
    check(full, sizeof full, 1, 0);

    static const char drm[] = "change@/drm\0ACTION=change\0SUBSYSTEM=drm\0";
    static const char pci[] = "add@/pci\0SUBSYSTEM=pci\0MODALIAS=test:queued\0";
    static const char other[] = "remove@/input\0ACTION=remove\0SUBSYSTEM=input\0";
    const struct packet queue[] = {
        { drm, sizeof drm - 1 }, { pci, sizeof pci - 1 }, { other, sizeof other - 1 },
    };
    check_packets(queue, sizeof queue / sizeof queue[0], 1, 1);
    assert(strcmp(matched[0], "test:queued") == 0);
    static const char lease[] = "change@/drm\0ACTION=change\0SUBSYSTEM=drm\0LEASE=1\0";
    static const char hotplug[] = "change@/drm\0ACTION=change\0SUBSYSTEM=drm\0HOTPLUG=1\0";
    const struct packet leased[] = { { lease, sizeof lease - 1 }, { lease, sizeof lease - 1 } };
    check_packets(leased, 2, 0, 0);
    const struct packet both[] = { { lease, sizeof lease - 1 }, { hotplug, sizeof hotplug - 1 } };
    check_packets(both, 2, 1, 0);
    check_packets(NULL, 0, 0, 0);
    receive_error = 0;
    check_packets(NULL, 0, 0, 0);
    receive_error = EBADF;
    check_packets(NULL, 0, 0, 0);
    puts("modules_event: DRM topology (leases and replays are not), alias loading, queue drain and bounds passed.");
    return 0;
}
