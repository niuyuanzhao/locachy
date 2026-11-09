#!/usr/bin/env python3
"""Actual init boot-storage wait with time, devices and netlink calls doubled.
No host block devices, sockets, module loading or real sleeps are used.
  python3 -B tools/test-init-storage.py [SCRATCH_PARENT]
Requires a C compiler (cc).
"""
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv[1] if len(sys.argv) > 1 else None


def function(source, declaration):
    start = source.index(declaration)
    return source[start:source.index("\n}\n", start) + 3]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/netlink.h>
#else
/* Only argument shapes: the fixture never opens a real socket. */
#define AF_NETLINK 16
#define NETLINK_KOBJECT_UEVENT 15
struct sockaddr_nl { unsigned short nl_family, nl_pad; unsigned int nl_pid, nl_groups; };
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 00004000
#endif

#define EVENT_FD 17
static int boot_dev_ready;
static double clock_ms, ready_at;
static int socket_fails, bind_fails, module_calls, socket_calls, bind_calls, close_calls;
static int scan_calls, poll_calls, sleep_calls, recv_calls, batch_reads, max_batch_reads;
static int interrupt_once, poll_error_once, recv_overflow_once, flood, finite_event;
static char messages[1024];

static double now_ms(void) { return clock_ms; }
static void say(const char *fmt, ...) {
    size_t used = strlen(messages);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(messages + used, sizeof messages - used, fmt, ap);
    va_end(ap);
    used = strlen(messages);
    assert(used + 2 < sizeof messages);
    strcpy(messages + used, "\n");
}
static int fixture_socket(int domain, int type, int protocol) {
    socket_calls++;
    assert(domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT);
    assert(type == (SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK));
    assert(!module_calls); /* Subscribe before loading early storage drivers. */
    if (socket_fails) { errno = EAFNOSUPPORT; return -1; }
    return EVENT_FD;
}
static int fixture_bind(int fd, const struct sockaddr *address, socklen_t size) {
    const struct sockaddr_nl *addr = (const struct sockaddr_nl *)address;
    bind_calls++;
    assert(fd == EVENT_FD && size == sizeof *addr);
    assert(addr->nl_family == AF_NETLINK && addr->nl_groups == 1);
    assert(!module_calls);
    if (bind_fails) { errno = EPERM; return -1; }
    return 0;
}
static int fixture_close(int fd) {
    assert(fd == EVENT_FD);
    assert(++close_calls == 1); /* No leak or double close. */
    return 0;
}
static void load_early_modules(const char *const *mods) {
    assert(socket_calls == 1 && bind_calls == !socket_fails);
    assert(mods[0] && !strcmp(mods[0], "vmd"));
    assert(mods[1] && !strcmp(mods[1], "nvme"));
    module_calls++;
}
static int check_boot_dev(const char *dev) {
    assert(!strcmp(dev, "/fixture/root"));
    if (ready_at >= 0 && clock_ms >= ready_at) boot_dev_ready = 1;
    return boot_dev_ready;
}
static void each_block_dev(int (*fn)(const char *)) {
    assert(fn == check_boot_dev && module_calls == 1);
    assert(++scan_calls < 1000); /* A faulty wait must fail rather than hang the runner. */
    if (batch_reads > max_batch_reads) max_batch_reads = batch_reads;
    assert(batch_reads <= 64); /* Unrelated traffic cannot indefinitely defer a rescan. */
    batch_reads = 0;
    fn("/fixture/root");
}
static int fixture_usleep(useconds_t amount) {
    assert(socket_fails || bind_fails || poll_error_once == -1);
    assert(amount > 0 && amount <= 50000);
    assert(++sleep_calls <= 201);
    clock_ms += amount / 1000.0;
    return 0;
}
static int fixture_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 1 && fds[0].fd == EVENT_FD && fds[0].events == POLLIN);
    assert(timeout > 0 && timeout <= 10000);
    assert(++poll_calls < 1000);
    fds[0].revents = 0;
    if (interrupt_once) {
        interrupt_once = 0;
        clock_ms += 100;
        errno = EINTR;
        return -1;
    }
    if (poll_error_once == 1) {
        poll_error_once = -1;
        clock_ms += 100;
        fds[0].revents = POLLERR;
        return 1;
    }
    if (flood) {
        clock_ms += 1;
        fds[0].revents = POLLIN;
        return 1;
    }
    if (ready_at >= 0 && ready_at <= clock_ms + timeout) {
        clock_ms = ready_at;
        finite_event = 1;
        fds[0].revents = POLLIN;
        return 1;
    }
    clock_ms += timeout;
    return 0;
}
static ssize_t fixture_recv(int fd, void *buffer, size_t size, int flags) {
    assert(fd == EVENT_FD && size == 4096 && flags == MSG_DONTWAIT);
    assert(++recv_calls < 20000);
    assert(++batch_reads <= 65); /* Removing the production cap fails promptly. */
    if (recv_overflow_once) {
        recv_overflow_once = 0;
        errno = ENOBUFS;
        return -1;
    }
    if (flood) {
        clock_ms += 1;
        memset(buffer, 'x', size); /* Deliberately unrelated, untrusted event data. */
        return (ssize_t)size;
    }
    if (finite_event) {
        finite_event = 0;
        memset(buffer, 'x', size);
        return (ssize_t)size;
    }
    errno = EAGAIN;
    return -1;
}
#define socket fixture_socket
#define bind fixture_bind
#define close fixture_close
#define usleep fixture_usleep
#define poll fixture_poll
#define recv fixture_recv
'''

SUFFIX = r'''
static void reset(void) {
    boot_dev_ready = 0;
    clock_ms = 0;
    ready_at = -1;
    socket_fails = bind_fails = module_calls = socket_calls = bind_calls = close_calls = 0;
    scan_calls = poll_calls = sleep_calls = recv_calls = batch_reads = max_batch_reads = 0;
    interrupt_once = poll_error_once = recv_overflow_once = flood = finite_event = 0;
    messages[0] = '\0';
}
static void cleanup_checked(void) {
    assert(module_calls == 1 && socket_calls == 1);
    assert(bind_calls == !socket_fails && close_calls == !socket_fails);
}
int main(void) {
    reset(); ready_at = 0;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 0 && scan_calls == 1);
    assert(!poll_calls && !sleep_calls && !recv_calls && !messages[0]);
    puts("PASS immediate boot storage adds no waiting or polling");

    reset(); ready_at = 3000;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3000 && scan_calls == 2);
    assert(poll_calls == 1 && recv_calls == 2 && !sleep_calls);
    assert(strstr(messages, "(uevents)") && strstr(messages, "boot storage ready after 3.000 s"));
    puts("PASS block event after 3 s discovers storage beyond the old 1.5 s window");

    reset(); bind_fails = 1; ready_at = 3200;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3200 && sleep_calls == 64);
    assert(!poll_calls && !recv_calls && strstr(messages, "(polling)"));
    puts("PASS netlink bind failure falls back to bounded polling and closes its socket");

    reset(); socket_fails = 1; ready_at = 3000;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3000 && sleep_calls == 60);
    assert(!poll_calls && !recv_calls && strstr(messages, "(polling)"));
    puts("PASS netlink socket failure falls back to polling without closing an invalid fd");

    reset(); interrupt_once = 1; ready_at = 3000;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3000 && poll_calls == 2 && !sleep_calls);
    assert(scan_calls == 3 && strstr(messages, "(uevents)"));
    puts("PASS EINTR preserves the subscription and original deadline before retrying");

    reset();
    prepare_boot_storage(); cleanup_checked();
    assert(!boot_dev_ready && clock_ms == 10000 && poll_calls == 1 && scan_calls == 2);
    assert(!sleep_calls && strstr(messages, "no boot storage found within 10 s"));
    puts("PASS absent disk ends at the ten-second deadline without periodic wakeups");

    reset(); bind_fails = 1;
    prepare_boot_storage(); cleanup_checked();
    assert(!boot_dev_ready && clock_ms == 10000 && sleep_calls == 200 && !poll_calls);
    assert(strstr(messages, "no boot storage found within 10 s"));
    puts("PASS fallback polling also ends at the ten-second deadline");

    reset(); flood = 1; ready_at = 3200;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms >= 3200 && clock_ms < 3265);
    assert(max_batch_reads == 64 && scan_calls > 2 && !sleep_calls);
    puts("PASS continuous unrelated events cannot starve a ready-device rescan (64-message cap)");

    reset(); flood = 1;
    prepare_boot_storage(); cleanup_checked();
    assert(!boot_dev_ready && clock_ms >= 10000 && clock_ms <= 10065);
    assert(max_batch_reads == 64 && scan_calls > 2 && strstr(messages, "no boot storage found within 10 s"));
    puts("PASS continuous unrelated events cannot starve the ten-second deadline");

    reset(); recv_overflow_once = 1; ready_at = 3000;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3000 && scan_calls == 2 && recv_calls == 1);
    assert(!sleep_calls);
    puts("PASS ENOBUFS leads to a device rescan rather than losing newly ready storage");

    reset(); poll_error_once = 1; ready_at = 3000;
    prepare_boot_storage(); cleanup_checked();
    assert(boot_dev_ready && clock_ms == 3000 && poll_calls == 1 && sleep_calls == 58);
    puts("PASS poll error closes netlink once and preserves the polling fallback deadline");
    return 0;
}
'''

source = (SOURCE / "init/init.c").read_text()
with tempfile.TemporaryDirectory(prefix="init-storage-", dir=SCRATCH) as tmp:
    root = Path(tmp)
    c = root / "test.c"
    c.write_text(PREFIX + function(source, "static void prepare_boot_storage(") + SUFFIX)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(root / "test")],
                   check=True, timeout=60)
    subprocess.run([str(root / "test")], check=True, timeout=30)
