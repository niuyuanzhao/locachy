/* The broker's lifecycle wrapper for the separate, keyless DNS-SD process. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef EXTDISPLAY_DISCOVERY_PATH
#define EXTDISPLAY_DISCOVERY_PATH "/usr/local/lib/locachy/extdisplay-advertise"
#endif

int extdisplay_discovery_start_ex(struct extdisplay_discovery *d, int netns, int ifindex,
                                  const char *name, const struct in6_addr *address,
                                  const char *code, const char *host_id, int paired_only)
{
    int pair[2];
    char index[16], ip[INET6_ADDRSTRLEN], parent[24];
    const char *wire_code = paired_only ? "PAIRED-ONLY" : code;
    if (!d || d->pid > 0 || netns < 0 || ifindex <= 0 || !name || !*name ||
        !address || !IN6_IS_ADDR_LINKLOCAL(address) || !wire_code || !*wire_code ||
        !inet_ntop(AF_INET6, address, ip, sizeof ip)) return -1;
    d->pid = -1;
    d->status = -1;
    snprintf(index, sizeof index, "%d", ifindex);
    snprintf(parent, sizeof parent, "%ld", (long)getpid());
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) return -1;
    pid_t owner = getpid();
    pid_t child = fork();
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != owner ||
            setns(netns, CLONE_NEWNET) < 0 || dup2(pair[1], 3) < 0) _exit(78);
        if (fcntl(3, F_SETFD, 0) < 0 || syscall(SYS_close_range, 4, ~0U, 0) < 0) _exit(78);
        char *args[12];
        int n = 0;
        args[n++] = EXTDISPLAY_DISCOVERY_PATH;
        if (host_id && *host_id) {
            args[n++] = "--host-id";
            args[n++] = (char *)host_id;
        }
        if (paired_only) {
            args[n++] = "--paired-only";
        }
        args[n++] = "--status-fd";
        args[n++] = "3";
        args[n++] = index;
        args[n++] = (char *)name;
        args[n++] = ip;
        args[n++] = parent;
        args[n] = NULL;
        char *const env[] = { "PATH=/usr/bin", "LC_ALL=C", NULL };
        execve(args[0], args, env);
        _exit(78);
    }
    close(pair[1]);
    if (child < 0) {
        close(pair[0]);
        return -1;
    }
    size_t code_len = strlen(wire_code);
    if (send(pair[0], wire_code, code_len, MSG_NOSIGNAL) != (ssize_t)code_len ||
        fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pair[0]);
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    d->pid = child;
    d->status = pair[0];
    return 0;
}

int extdisplay_discovery_start(struct extdisplay_discovery *d, int netns, int ifindex,
                               const char *name, const struct in6_addr *address, const char *code)
{
    return extdisplay_discovery_start_ex(d, netns, ifindex, name, address, code, NULL, 0);
}

int extdisplay_discovery_status(struct extdisplay_discovery *d)
{
    char reply[16];
    if (!d || d->status < 0) return -1;
    ssize_t n = recv(d->status, reply, sizeof reply, MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n == 5 && memcmp(reply, "ready", 5) == 0 ? 1 : -1;
}

void extdisplay_discovery_stop(struct extdisplay_discovery *d)
{
    if (!d) return;
    if (d->status >= 0) {
        /* A bounded stop message wakes Avahi before the namespace is removed. */
        (void)!send(d->status, "stop", 4, MSG_NOSIGNAL | MSG_DONTWAIT);
        shutdown(d->status, SHUT_WR);
        struct pollfd p = { d->status, POLLIN, 0 };
        for (int i = 0; i < 10; i++) {
            char discard[32];
            if (poll(&p, 1, 50) > 0 && recv(d->status, discard, sizeof discard, MSG_DONTWAIT) <= 0) break;
        }
        close(d->status);
    }
    if (d->pid > 0) {
        if (waitpid(d->pid, NULL, WNOHANG) == 0) kill(d->pid, SIGKILL);
        while (waitpid(d->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    d->pid = -1;
    d->status = -1;
}
