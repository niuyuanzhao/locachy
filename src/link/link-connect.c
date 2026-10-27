/*
 * link-connect ADDRESS PORT PROGRAM [ARG...]: connect to ADDRESS (an IPv6
 * address, with %INTERFACE for a link-local one) at PORT, and run PROGRAM
 * with the connection as its standard input, output and error. For the cable
 * debug link (link-client), whose shell must notice when the other end is
 * gone: bash's /dev/tcp cannot set socket options, so a connection whose far
 * end vanished (it restarted, or a cable was down when it closed) would hold
 * the shell forever. Here TCP keepalive probes an idle connection (after
 * 10 s, every 5 s, 3 tries) and unacknowledged data times out after 30 s;
 * either way the shell's next read fails and it exits, and whoever runs this
 * starts it again.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void opt(int fd, int level, int name, int value)
{
    if (setsockopt(fd, level, name, &value, sizeof value) < 0) {
        fprintf(stderr, "link-connect: setsockopt %d: %s\n", name, strerror(errno));
    }
}

int main(int argc, char **argv)
{
    struct addrinfo hints = { .ai_family = AF_INET6, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST };
    struct addrinfo *ai;

    if (argc < 4) {
        fprintf(stderr, "usage: link-connect ADDRESS PORT PROGRAM [ARG...]\n");
        return 2;
    }
    int err = getaddrinfo(argv[1], argv[2], &hints, &ai);
    if (err != 0) {
        fprintf(stderr, "link-connect: %s: %s\n", argv[1], gai_strerror(err));
        return 1;
    }
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("link-connect: socket");
        return 1;
    }
    opt(fd, IPPROTO_TCP, TCP_SYNCNT, 2); /* give up connecting after about 7 s */
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        fprintf(stderr, "link-connect: connect %s port %s: %s\n", argv[1], argv[2], strerror(errno));
        return 1;
    }
    freeaddrinfo(ai);
    opt(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
    opt(fd, IPPROTO_TCP, TCP_KEEPIDLE, 10);
    opt(fd, IPPROTO_TCP, TCP_KEEPINTVL, 5);
    opt(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);
    opt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, 30000);

    int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kmsg >= 0) {
        dprintf(kmsg, "<6>link-client: connected to %s port %s\n", argv[1], argv[2]);
        close(kmsg);
    }
    for (int i = 0; i < 3; i++) {
        dup2(fd, i);
    }
    if (fd > 2) {
        close(fd);
    }
    execvp(argv[3], argv + 3);
    perror("link-connect: exec");
    return 127;
}
