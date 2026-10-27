/*
 * xdg-open [--with CONTAINER] TARGET (/usr/local/bin/xdg-open in every
 * container): hand a path or URL to the host, which opens it in the
 * container chosen for its type (the user's default, or the one the user
 * picks), or in CONTAINER. Paths mean the same file everywhere only in the
 * shared folders of the home; folders open in home's file manager.
 * Exit status as xdg-utils: 1 usage, 2 no such file, 3 no host to ask,
 * 4 the host could not open it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET "/run/locachy-open/socket"    /* in a container */
#define HOST_SOCKET "/run/locachy/open/socket" /* in a host program */

static int is_url(const char *s)
{
    if (!isalpha((unsigned char)*s)) {
        return 0;
    }
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.') {
        s++;
    }
    return *s == ':';
}

int main(int argc, char **argv)
{
    const char *with = NULL, *target;
    char path[PATH_MAX], request[PATH_MAX + 128], reply[512];

    if (argc == 4 && strcmp(argv[1], "--with") == 0) {
        with = argv[2];
        target = argv[3];
    } else if (argc == 2 && argv[1][0] != '-') {
        target = argv[1];
    } else {
        fprintf(stderr, "usage: xdg-open [--with CONTAINER] FILE|URL\n");
        return 1;
    }
    if (!is_url(target)) {
        if (realpath(target, path) == NULL) {
            perror(target);
            return 2;
        }
        target = path;
    }
    int n = with ? snprintf(request, sizeof request, "with %s %s", with, target)
                 : snprintf(request, sizeof request, "open %s", target);
    if (n < 0 || (size_t)n >= sizeof request) {
        fprintf(stderr, "xdg-open: too long\n");
        return 1;
    }

    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = SOCKET };
    if (access(SOCKET, F_OK) != 0) {
        snprintf(address.sun_path, sizeof address.sun_path, "%s", HOST_SOCKET);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        send(fd, request, (size_t)n, MSG_NOSIGNAL) != n) {
        fprintf(stderr, "xdg-open: the host is not listening (%s)\n", SOCKET);
        return 3;
    }
    /* The user may be choosing the program: wait for that. */
    struct pollfd wait = { .fd = fd, .events = POLLIN };
    ssize_t got = poll(&wait, 1, 5 * 60 * 1000) > 0 ? recv(fd, reply, sizeof reply - 1, 0) : -1;
    close(fd);
    if (got <= 0) {
        fprintf(stderr, "xdg-open: no answer from the host\n");
        return 4;
    }
    reply[got] = '\0';
    if (strncmp(reply, "ok", 2) != 0) {
        fprintf(stderr, "xdg-open: %s\n", strncmp(reply, "error ", 6) == 0 ? reply + 6 : reply);
        return 4;
    }
    return 0;
}
