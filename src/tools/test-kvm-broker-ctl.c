/* Test harness: starts kvm-broker as kvm does (a SOCK_SEQPACKET pair, the
 * broker's end as --control-fd 3, nothing else open) and relays for a test
 * script. stdin lines:
 *   > TEXT          send TEXT as one control message
 *   plain TEXT      write TEXT to the stream received with "paired"
 *   plain-close     close that stream
 *   kill            SIGKILL the broker
 *   close           close the control channel (EOF for the broker)
 * stdout lines: "pid N", "< TEXT" (with " +fd" when a descriptor came),
 * "plain< TEXT", "plain-eof", "control-eof", "exit N" or "signal N". */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int pair[2], control, plain = -1, input_open = 1;
    pid_t broker;
    char line[1024];
    size_t have = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: test-kvm-broker-ctl BROKER\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) return 1;
    control = pair[0];
    broker = fork();
    if (broker == 0) {
        /* stderr stays (the test reads the broker's log there). */
        if (dup2(pair[1], 3) < 0) _exit(127);
        int null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0) _exit(127);
        syscall(SYS_close_range, 4, ~0U, 0);
        execl(argv[1], "kvm-broker", "--control-fd", "3", (char *)NULL);
        _exit(127);
    }
    close(pair[1]);
    printf("pid %d\n", (int)broker);
    fflush(stdout);
    for (;;) {
        struct pollfd p[3] = { { input_open ? 0 : -1, POLLIN, 0 }, { control, POLLIN, 0 }, { plain, POLLIN, 0 } };
        poll(p, 3, 100);
        int status;
        if (broker > 0 && waitpid(broker, &status, WNOHANG) == broker) {
            if (WIFEXITED(status)) printf("exit %d\n", WEXITSTATUS(status));
            else printf("signal %d\n", WTERMSIG(status));
            fflush(stdout);
            broker = -1;
        }
        if (control >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char text[600];
            union {
                struct cmsghdr h;
                char space[CMSG_SPACE(4 * sizeof(int))];
            } u;
            struct iovec iov = { text, sizeof text - 1 };
            struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.space,
                                .msg_controllen = sizeof u.space };
            ssize_t n = recvmsg(control, &m, MSG_CMSG_CLOEXEC);
            if (n <= 0) {
                printf("control-eof\n");
                close(control);
                control = -1;
            } else {
                int fds = 0;
                text[n] = 0;
                for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                    int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    for (int i = 0; i < count; i++) {
                        int fd;
                        memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                        if (plain < 0) plain = fd;
                        else close(fd);
                        fds++;
                    }
                }
                printf("< %s%s\n", text, fds == 1 ? " +fd" : fds ? " +fds" : "");
            }
            fflush(stdout);
        }
        if (plain >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            char data[600];
            ssize_t n = read(plain, data, sizeof data - 1);
            if (n <= 0) {
                printf("plain-eof\n");
                close(plain);
                plain = -1;
            } else {
                data[n] = 0;
                data[strcspn(data, "\n")] = 0;
                printf("plain< %s\n", data);
            }
            fflush(stdout);
        }
        if (input_open && (p[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, line + have, sizeof line - 1 - have);
            if (n <= 0) {
                input_open = 0;
                if (control >= 0) close(control);
                control = -1;
                continue;
            }
            have += (size_t)n;
            char *nl;
            while ((nl = memchr(line, '\n', have))) {
                *nl = 0;
                if (!strncmp(line, "> ", 2) && control >= 0) {
                    (void)!send(control, line + 2, strlen(line + 2), MSG_NOSIGNAL);
                } else if (!strncmp(line, "plain ", 6) && plain >= 0) {
                    (void)!write(plain, line + 6, strlen(line + 6));
                } else if (!strcmp(line, "plain-close") && plain >= 0) {
                    close(plain);
                    plain = -1;
                } else if (!strcmp(line, "kill") && broker > 0) {
                    kill(broker, SIGKILL);
                } else if (!strcmp(line, "close") && control >= 0) {
                    close(control);
                    control = -1;
                }
                have -= (size_t)(nl + 1 - line);
                memmove(line, nl + 1, have);
            }
        }
        if (!input_open && broker < 0 && control < 0) return 0;
    }
}
