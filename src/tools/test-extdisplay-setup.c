/*
 * The pairing client (progs/extdisplay-setup.c) against a stub broker on a real
 * SOCK_SEQPACKET channel: the reply sequences, the refusals, the descriptor
 * rules, the wiping of the code. Linux only (the client uses MSG_NOSIGNAL and
 * SOCK_CLOEXEC). Build and run by tools/test-extdisplay.sh.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-setup.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The stub broker's side: one message with n descriptors (none when 0). */
static int reply_len(int s, const char *text, size_t len, const int *fds, int n)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2 * sizeof(int))]; } c;
    struct iovec iov = { (void *)text, len };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    memset(&c, 0, sizeof c);
    if (n) {
        msg.msg_control = c.bytes;
        msg.msg_controllen = CMSG_SPACE(n * sizeof(int));
        struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(n * sizeof(int));
        memcpy(CMSG_DATA(h), fds, n * sizeof(int));
    }
    return sendmsg(s, &msg, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -1;
}
static int reply(int s, const char *text) { return reply_len(s, text, strlen(text), NULL, 0); }

/* What the client sent: its message, or "" when there is none. */
static const char *request(int s)
{
    static char buf[600];
    ssize_t n = recv(s, buf, sizeof buf - 1, MSG_DONTWAIT);
    buf[n < 0 ? 0 : n] = '\0';
    return buf;
}

static int channel(int sv[2]) { return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv); }

/* A stream and a datagram socket, for the descriptor rules. */
static int stream_pair(int sv[2]) { return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv); }

static void flow(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    CHECK(extdisplay_setup_pair(&s, "eth0") == -1);          /* not listed yet, not in LIST */
    CHECK(extdisplay_setup_scan(&s) == 0 && s.state == EXTDISPLAY_SETUP_SCANNING);
    CHECK(strcmp(request(sv[1]), "scan") == 0);
    CHECK(reply(sv[1], "iface eth0 e1000e up") == 0);
    CHECK(reply(sv[1], "iface wlan0 iwlwifi down") == 0);
    CHECK(reply(sv[1], "end\n") == 0);                /* one trailing newline is accepted */
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_LIST && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(s.count == 2 && strcmp(s.ifaces[0].name, "eth0") == 0 && strcmp(s.ifaces[0].driver, "e1000e") == 0);
    CHECK(s.ifaces[0].carrier == 1 && s.ifaces[1].carrier == 0);
    CHECK(extdisplay_setup_pair(&s, "enp9s0") == -1);        /* not listed */
    CHECK(extdisplay_setup_pair(&s, "eth 0") == -1);         /* not an interface name */
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0 && s.state == EXTDISPLAY_SETUP_PAIRING);
    CHECK(strcmp(request(sv[1]), "pair eth0") == 0);
    CHECK(reply(sv[1], "lease eth0 fe80::a8bb:ccff:fedd:eeff 27183") == 0);
    CHECK(reply(sv[1], "code ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YQ") == 0);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_CODE && s.state == EXTDISPLAY_SETUP_WAITING);
    CHECK(strcmp(s.linklocal, "fe80::a8bb:ccff:fedd:eeff") == 0 && strcmp(s.port, "27183") == 0);
    CHECK(strcmp(s.code, "ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YQ") == 0);
    int pair[2];
    CHECK(stream_pair(pair) == 0);
    CHECK(reply_len(sv[1], "paired", 6, &pair[1], 1) == 0);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_PAIRED && s.state == EXTDISPLAY_SETUP_PAIRED);
    CHECK(s.stream >= 0 && s.code[0] == '\0' && s.linklocal[0] == '\0' && s.port[0] == '\0');
    if (s.stream >= 0) close(s.stream);
    close(pair[0]);
    close(pair[1]);
    close(sv[0]);
    close(sv[1]);
}

static void cancelled(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply(sv[1], "iface eth0 e1000e up");
    reply(sv[1], "end");
    extdisplay_setup_read(&s);
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    reply(sv[1], "lease eth0 fe80::1%eth0 9000");
    reply(sv[1], "code ABCDE-FGHIJ");
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_CODE);
    CHECK(extdisplay_setup_cancel(&s) == 0 && s.state == EXTDISPLAY_SETUP_CANCELLING && s.code[0] == '\0');
    CHECK(strcmp(request(sv[1]), "cancel") == 0);
    reply(sv[1], "ended cancelled");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(strcmp(s.reason, "cancelled") == 0 && s.linklocal[0] == '\0');
    /* A pairing that completes as the cancel goes out: its stream is refused. */
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    reply(sv[1], "lease eth0 fe80::1%eth0 9000");
    reply(sv[1], "code ABCDE-FGHIJ");
    extdisplay_setup_read(&s);
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_cancel(&s) == 0);
    request(sv[1]);
    int pair[2];
    CHECK(stream_pair(pair) == 0);
    reply_len(sv[1], "paired", 6, &pair[1], 1);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE && s.state == EXTDISPLAY_SETUP_CANCELLING && s.stream == -1);
    reply(sv[1], "ended cancelled");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    close(pair[0]);
    close(pair[1]);
    close(sv[0]);
    close(sv[1]);
}

static void released(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply(sv[1], "end");
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_release(&s) == 0 && s.state == EXTDISPLAY_SETUP_RELEASING);
    CHECK(strcmp(request(sv[1]), "release") == 0);
    reply(sv[1], "ended carrier-lost");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE && s.state == EXTDISPLAY_SETUP_RELEASING);
    reply(sv[1], "ended released");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_RELEASED);
    CHECK(extdisplay_setup_release(&s) == -1);
    close(sv[1]);                                   /* the broker exits */
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_RELEASED);
    close(sv[0]);
}

/* The broker answers "pair" before it reads a cancel sent meanwhile: the
 * lease and code (or the error) come first, the cancel's answer after. */
static void crossing(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply(sv[1], "iface eth0 e1000e up");
    reply(sv[1], "end");
    extdisplay_setup_read(&s);
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    CHECK(extdisplay_setup_cancel(&s) == 0 && s.state == EXTDISPLAY_SETUP_CANCELLING);
    request(sv[1]);
    reply(sv[1], "lease eth0 fe80::1%eth0 9000");
    reply(sv[1], "code ABCDE-FGHIJ");
    reply(sv[1], "ended cancelled");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE && s.state == EXTDISPLAY_SETUP_CANCELLING);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_NONE && s.state == EXTDISPLAY_SETUP_CANCELLING);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(s.linklocal[0] == '\0' && s.code[0] == '\0');
    /* A pair refused as the cancel goes out: the error is the attempt's end. */
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    CHECK(extdisplay_setup_cancel(&s) == 0);
    request(sv[1]);
    reply(sv[1], "error no link on eth0 yet");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(strcmp(s.reason, "cancelled") == 0);
    close(sv[0]);
    close(sv[1]);
    /* A scan refused while a lease is active: an error shown, not a broken channel. */
    extdisplay_setup_init(&s, -1);
    s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "error busy", NULL, 0) == EXTDISPLAY_SETUP_EV_SCAN_ERROR && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(strcmp(s.error, "busy") == 0 && s.count == 0);
    /* A lease with another name, while cancelling, is still a broken channel. */
    extdisplay_setup_init(&s, -1);
    s.state = EXTDISPLAY_SETUP_CANCELLING;
    snprintf(s.pairing, sizeof s.pairing, "%s", "eth0");
    CHECK(extdisplay_setup_line(&s, "lease eth1 fe80::1%eth1 9000", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
}

static void errors_and_eof(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply(sv[1], "end");
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_pair(&s, "eth0") == -1 && s.state == EXTDISPLAY_SETUP_LIST); /* nothing listed */
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply(sv[1], "iface eth0 e1000e up");
    reply(sv[1], "end");
    extdisplay_setup_read(&s);
    extdisplay_setup_read(&s);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    reply(sv[1], "error no link on eth0 yet, plug the cable");
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_ERROR && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(strcmp(s.error, "no link on eth0 yet, plug the cable") == 0);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    request(sv[1]);
    close(sv[1]);                                   /* the broker is gone mid-pairing */
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_BROKEN && s.state == EXTDISPLAY_SETUP_BROKEN);
    close(sv[0]);
}

/* Lines that must end the channel, each in the state it arrives in. */
static void refusals(void)
{
    struct extdisplay_setup s;
    int fds[2];
    extdisplay_setup_init(&s, -1);
    s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "iface eth0 e1000e sideways", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "iface bad!name e1000e up", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "iface eth0 e1000e", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "end extra", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "frobnicate", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_IDLE;
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    CHECK(s.state == EXTDISPLAY_SETUP_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(stream_pair(fds) == 0);
    CHECK(extdisplay_setup_line(&s, "iface eth0 e1000e up", &fds[0], 1) == EXTDISPLAY_SETUP_EV_BROKEN);
    close(fds[1]);

    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    for (int i = 0; i < 20; i++) {
        char line[64];
        snprintf(line, sizeof line, "iface n%d e1000e up", i);
        CHECK(extdisplay_setup_line(&s, line, NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    }
    CHECK(s.count == EXTDISPLAY_SETUP_IFACES && s.state == EXTDISPLAY_SETUP_SCANNING);

    struct { const char *line; } lease_bad[] = {
        { "lease eth1 fe80::1%eth0 9000" }, { "lease eth0 fe80::1 0" }, { "lease eth0 fe80::1 65536" },
        { "lease eth0 fe80::1 09" }, { "lease eth0 fe80::1 9000 extra" }, { "lease eth0 fe80::1/64 9000" },
    };
    for (size_t i = 0; i < sizeof lease_bad / sizeof lease_bad[0]; i++) {
        extdisplay_setup_init(&s, -1);
        s.state = EXTDISPLAY_SETUP_PAIRING;
        snprintf(s.pairing, sizeof s.pairing, "%s", "eth0");
        CHECK(extdisplay_setup_line(&s, lease_bad[i].line, NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    }

    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_PAIRING; snprintf(s.pairing, sizeof s.pairing, "%s", "eth0");
    CHECK(extdisplay_setup_line(&s, "code ABCDE-FGHIJ", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    const char *codes[] = { "abcde", "ABCD1", "-ABCDE", "ABCDE--FGHIJ", "ABCDE-", "ABCDE FGHIJ" };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_PAIRING; snprintf(s.pairing, sizeof s.pairing, "%s", "eth0");
        CHECK(extdisplay_setup_line(&s, "lease eth0 fe80::1%eth0 9000", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
        char line[80];
        snprintf(line, sizeof line, "code %s", codes[i]);
        CHECK(extdisplay_setup_line(&s, line, NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    }
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_PAIRING; snprintf(s.pairing, sizeof s.pairing, "%s", "eth0");
    CHECK(extdisplay_setup_line(&s, "error ", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_PAIRING;
    CHECK(extdisplay_setup_line(&s, "paired", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);

    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_WAITING;
    CHECK(stream_pair(fds) == 0);
    int two[2] = { fds[0], fds[1] };
    CHECK(extdisplay_setup_line(&s, "paired", two, 2) == EXTDISPLAY_SETUP_EV_BROKEN);
    int dgram[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, dgram) == 0);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_WAITING;
    CHECK(extdisplay_setup_line(&s, "paired", &dgram[0], 1) == EXTDISPLAY_SETUP_EV_BROKEN);
    close(dgram[1]);

    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_WAITING;
    CHECK(extdisplay_setup_line(&s, "ended bogus", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_WAITING;
    CHECK(extdisplay_setup_line(&s, "frobnicate", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_LIST; s.scanned = 1;
    CHECK(extdisplay_setup_line(&s, "ended cancelled", NULL, 0) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_WAITING;           /* nothing scanned: back to idle */
    CHECK(extdisplay_setup_line(&s, "ended timeout", NULL, 0) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_IDLE);
    /* A late "ended cancelled" during the rescan that followed an unsolicited end. */
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "ended cancelled", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE && s.state == EXTDISPLAY_SETUP_SCANNING);
    /* The session ended here first: cancel is allowed while paired. */
    extdisplay_setup_init(&s, -1); s.state = EXTDISPLAY_SETUP_PAIRED;
    CHECK(extdisplay_setup_cancel(&s) == -1 && s.state == EXTDISPLAY_SETUP_BROKEN); /* no channel to send on */
}

/* Over the channel: an overlong line, and a line with a control character. */
static void framing(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    char big[EXTDISPLAY_SETUP_LINE + 2];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply_len(sv[1], big, EXTDISPLAY_SETUP_LINE + 1, NULL, 0);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_BROKEN && s.state == EXTDISPLAY_SETUP_BROKEN);
    close(sv[0]);
    close(sv[1]);
    CHECK(channel(sv) == 0);
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_setup_scan(&s);
    request(sv[1]);
    reply_len(sv[1], "iface eth0\x07 e1000e up", 23, NULL, 0);
    CHECK(extdisplay_setup_read(&s) == EXTDISPLAY_SETUP_EV_BROKEN);
    close(sv[0]);
    close(sv[1]);
}

/* Run as the broker by extdisplay_setup_spawn_broker ("--control-fd 3"): checks that
 * nothing but stdio and fd 3 came along, then answers as the broker does. */
static int stub_send(const char *text)
{
    return send(EXTDISPLAY_BROKER_FD, text, strlen(text), MSG_NOSIGNAL) == (ssize_t)strlen(text) ? 0 : -1;
}

static int stub(void)
{
    for (int fd = EXTDISPLAY_BROKER_FD + 1; fd < 256; fd++) {
        if (fcntl(fd, F_GETFD) != -1) return 3; /* a descriptor leaked into the broker */
    }
    int type = 0;
    socklen_t size = sizeof type;
    if (getsockopt(EXTDISPLAY_BROKER_FD, SOL_SOCKET, SO_TYPE, &type, &size) < 0 || type != SOCK_SEQPACKET) return 4;
    int pairs = 0, held = -1;
    for (;;) {
        char buf[600];
        ssize_t n = recv(EXTDISPLAY_BROKER_FD, buf, sizeof buf - 1, 0);
        if (n <= 0) return held >= 0 ? 6 : 0;
        buf[n] = '\0';
        if (strcmp(buf, "scan") == 0) {
            stub_send("iface eth0 thunderbolt-net up");
            stub_send("iface eth1 thunderbolt-net down");
            stub_send("end");
        } else if (strcmp(buf, "pair eth0") == 0) {
            stub_send("lease eth0 fe80::a8bb:ccff:fedd:eeff 27183");
            stub_send("code ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YQ");
            if (++pairs == 2) {
                int sv[2];
                if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return 7;
                if (reply_len(EXTDISPLAY_BROKER_FD, "paired", 6, &sv[1], 1) < 0) return 8;
                close(sv[1]);
                held = sv[0];
            }
        } else if (strcmp(buf, "cancel") == 0) {
            stub_send("ended cancelled");
        } else if (strcmp(buf, "release") == 0) {
            char got[8] = "";
            if (held < 0 || recv(held, got, 4, MSG_DONTWAIT) != 4 || memcmp(got, "ping", 4) != 0) return 5;
            stub_send("ended released");
            close(held);
            return 0;
        } else {
            stub_send("error unknown command");
        }
    }
}

static enum extdisplay_setup_event wait_event(struct extdisplay_setup *s)
{
    for (int i = 0; i < 300; i++) {
        struct pollfd p = { s->fd, POLLIN, 0 };
        if (poll(&p, 1, 10) > 0) {
            enum extdisplay_setup_event e = extdisplay_setup_read(s);
            if (e != EXTDISPLAY_SETUP_EV_NONE) return e;
        }
    }
    return EXTDISPLAY_SETUP_EV_NONE;
}

/* The broker as a real child: spawned with fd 5 open (the session owner's
 * place), scan, pair, cancel, pair to the stream, bytes through it, release. */
static void spawned(const char *self)
{
    int owner = open("/dev/null", O_RDONLY), ctl = -1, status = -1;
    if (owner >= 0 && owner != 5) {
        dup2(owner, 5);
        close(owner);
        owner = 5;
    }
    pid_t pid = extdisplay_setup_spawn_broker(self, &ctl);
    CHECK(owner == 5 && pid > 0 && ctl >= 0);
    if (pid <= 0) return;
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, ctl);
    CHECK(extdisplay_setup_scan(&s) == 0);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_LIST && s.count == 2 && s.ifaces[1].carrier == 0);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_CODE && strcmp(s.port, "27183") == 0);
    CHECK(extdisplay_setup_cancel(&s) == 0);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(extdisplay_setup_pair(&s, "eth0") == 0);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_CODE);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_PAIRED && s.stream >= 0);
    if (s.stream >= 0) {
        CHECK(write(s.stream, "ping", 4) == 4);
        close(s.stream);
    }
    CHECK(extdisplay_setup_release(&s) == 0);
    CHECK(wait_event(&s) == EXTDISPLAY_SETUP_EV_ENDED && s.state == EXTDISPLAY_SETUP_RELEASED);
    close(ctl);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (WIFEXITED(status) && WEXITSTATUS(status)) printf("stub broker exited %d\n", WEXITSTATUS(status));
    close(owner);
}

static void automatic_flow(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    struct extdisplay_setup s;
    extdisplay_setup_init(&s, sv[0]);
    CHECK(extdisplay_setup_watch(&s) == 0);
    CHECK(!strcmp(request(sv[1]), "watch"));
    CHECK(extdisplay_setup_line(&s, "iface cable0 thunderbolt-net down 8 02:00:00:00:00:01", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    CHECK(extdisplay_setup_automatic(&s) == 0); /* bring up the only candidate even without carrier */
    CHECK(extdisplay_setup_pair(&s, "cable0") == 0);
    CHECK(!strcmp(request(sv[1]), "pair cable0 8 02:00:00:00:00:01"));
    CHECK(extdisplay_setup_line(&s, "preparing cable0 carrier", NULL, 0) == EXTDISPLAY_SETUP_EV_PROGRESS);
    CHECK(!strcmp(s.progress, "carrier"));
    CHECK(extdisplay_setup_line(&s, "preparing cable0 address", NULL, 0) == EXTDISPLAY_SETUP_EV_PROGRESS);
    CHECK(extdisplay_setup_line(&s, "preparing cable0 discovery", NULL, 0) == EXTDISPLAY_SETUP_EV_PROGRESS);
    CHECK(extdisplay_setup_cancel(&s) == 0);
    CHECK(extdisplay_setup_line(&s, "preparing cable0 listener", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "ended cancelled", NULL, 0) == EXTDISPLAY_SETUP_EV_ENDED);
    CHECK(extdisplay_setup_line(&s, "changed", NULL, 0) == EXTDISPLAY_SETUP_EV_CHANGED);
    CHECK(s.changed && extdisplay_setup_automatic(&s) == -1);
    CHECK(extdisplay_setup_pair(&s, "cable0") == -1); /* stale list cannot select a new same-name device */
    CHECK(extdisplay_setup_scan(&s) == 0);
    CHECK(extdisplay_setup_line(&s, "iface cable0 thunderbolt-net down 9 02:00:00:00:00:02", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "iface cable1 thunderbolt-net up 10 02:00:00:00:00:03", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    CHECK(extdisplay_setup_automatic(&s) == 1);
    s.ifaces[0].carrier = 1;
    CHECK(extdisplay_setup_automatic(&s) == -1); /* multiple live links need a choice */
    s.ifaces[0].carrier = s.ifaces[1].carrier = 0;
    CHECK(extdisplay_setup_automatic(&s) == -1);
    close(sv[0]);
    close(sv[1]);
    extdisplay_setup_init(&s, -1);
    CHECK(extdisplay_setup_line(&s, "changed", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1);
    s.watching = 1; s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "iface cable0 thunderbolt-net up 0 02:00", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1);
    s.watching = 1; s.state = EXTDISPLAY_SETUP_SCANNING;
    CHECK(extdisplay_setup_line(&s, "iface cable0 thunderbolt-net up 8 invalid", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
    extdisplay_setup_init(&s, -1);
    s.state = EXTDISPLAY_SETUP_PAIRING; strcpy(s.pairing, "cable0");
    CHECK(extdisplay_setup_line(&s, "preparing cable1 carrier", NULL, 0) == EXTDISPLAY_SETUP_EV_BROKEN);
}

static void auto_reply(struct extdisplay_auto *a, struct extdisplay_setup *s, const char *line,
                       enum extdisplay_setup_event expected, double now)
{
    enum extdisplay_setup_event ev = extdisplay_setup_line(s, line, NULL, 0);
    CHECK(ev == expected);
    extdisplay_auto_on_event(a, s, ev, now);
}

static void auto_list(struct extdisplay_auto *a, struct extdisplay_setup *s, const char *iface, double now)
{
    if (iface) auto_reply(a, s, iface, EXTDISPLAY_SETUP_EV_NONE, now);
    auto_reply(a, s, "end", EXTDISPLAY_SETUP_EV_LIST, now);
}

static void auto_request(struct extdisplay_auto *a, struct extdisplay_setup *s, int broker, double now,
                         const char *expected)
{
    CHECK(extdisplay_auto_step(a, s, now) == (*expected != '\0'));
    CHECK(strcmp(request(broker), expected) == 0);
}

static const char tb_up[] = "iface tb0 thunderbolt-net up 8 02:00:00:00:00:01";
static const char tb_down[] = "iface tb0 thunderbolt-net down 8 02:00:00:00:00:01";
static const char tb_pair[] = "pair tb0 8 02:00:00:00:00:01";
static const char prep_address[] = "error the cable has carrier but no usable IPv6 link-local address";

static void scan_recovery(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    struct extdisplay_auto a;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_auto_init(&a);
    CHECK(extdisplay_setup_watch(&s) == 0);
    CHECK(!strcmp(request(sv[1]), "watch"));

    /* Scan errors never immediately scan again or suppress a future pair.
     * A refused initial watch did not subscribe on the broker side. */
    auto_reply(&a, &s, "error busy", EXTDISPLAY_SETUP_EV_SCAN_ERROR, 10.0);
    auto_request(&a, &s, sv[1], 10.0, "");
    auto_request(&a, &s, sv[1], 10.49, "");
    auto_request(&a, &s, sv[1], 10.5, "watch");
    auto_reply(&a, &s, "error busy", EXTDISPLAY_SETUP_EV_SCAN_ERROR, 10.5);
    auto_request(&a, &s, sv[1], 11.49, "");
    auto_request(&a, &s, sv[1], 11.5, "watch");
    auto_reply(&a, &s, "error busy", EXTDISPLAY_SETUP_EV_SCAN_ERROR, 11.5);
    auto_request(&a, &s, sv[1], 13.49, "");
    auto_request(&a, &s, sv[1], 13.5, "watch");
    auto_reply(&a, &s, "error busy", EXTDISPLAY_SETUP_EV_SCAN_ERROR, 13.5);
    auto_request(&a, &s, sv[1], 1000.0, "");
    CHECK(s.count == 0 && !s.watching && !a.suppressed && !strcmp(s.error, "busy"));

    /* F5 can retry the initial subscription after the bounded burst ends. */
    extdisplay_auto_pause(&a, 1);
    CHECK(extdisplay_setup_scan(&s) == 0);
    CHECK(!strcmp(request(sv[1]), "watch"));
    auto_list(&a, &s, NULL, 1001.0);
    CHECK(s.watching && !s.watch_pending && !s.error[0]);
    auto_request(&a, &s, sv[1], 2000.0, "");
    auto_reply(&a, &s, "changed", EXTDISPLAY_SETUP_EV_CHANGED, 2000.0);
    auto_request(&a, &s, sv[1], 2000.0, "scan");
    auto_reply(&a, &s, "error busy", EXTDISPLAY_SETUP_EV_SCAN_ERROR, 2000.0);
    auto_request(&a, &s, sv[1], 2000.49, "");
    auto_request(&a, &s, sv[1], 2000.5, "scan");
    auto_list(&a, &s, tb_up, 2000.5);
    auto_request(&a, &s, sv[1], 2000.5, tb_pair);

    /* A timeout waiting for a sender is not an authentication failure: keep
     * the established long-running availability on the same connected link. */
    auto_reply(&a, &s, "lease tb0 fe80::1 27183", EXTDISPLAY_SETUP_EV_NONE, 2001.0);
    auto_reply(&a, &s, "code ABCD-EFGH", EXTDISPLAY_SETUP_EV_CODE, 2001.0);
    auto_reply(&a, &s, "ended timeout", EXTDISPLAY_SETUP_EV_ENDED, 2301.0);
    CHECK(!strcmp(request(sv[1]), "scan"));
    auto_list(&a, &s, tb_up, 2301.0);
    auto_request(&a, &s, sv[1], 2301.0, tb_pair);
    close(sv[0]);
    close(sv[1]);
}

static void preparation_recovery(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    struct extdisplay_auto a;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_auto_init(&a);
    CHECK(extdisplay_setup_watch(&s) == 0);
    CHECK(!strcmp(request(sv[1]), "watch"));
    auto_list(&a, &s, tb_up, 0.0);
    auto_request(&a, &s, sv[1], 0.0, tb_pair);

    /* A stale lease and two empty snapshots, without any netlink event.
     * The original name, identity and carrier are unchanged on return. */
    auto_reply(&a, &s, "error the previous lease of this device did not end cleanly; it is not back in locachy-net yet",
               EXTDISPLAY_SETUP_EV_ERROR, 20.0);
    auto_request(&a, &s, sv[1], 20.49, "");
    auto_request(&a, &s, sv[1], 20.5, "scan");
    auto_list(&a, &s, NULL, 20.5);
    CHECK(s.error[0] && s.count == 0);
    auto_request(&a, &s, sv[1], 21.49, "");
    auto_request(&a, &s, sv[1], 21.5, "scan");
    auto_list(&a, &s, NULL, 21.5);
    auto_request(&a, &s, sv[1], 23.49, "");
    auto_request(&a, &s, sv[1], 23.5, "scan");
    auto_list(&a, &s, tb_up, 23.5);
    auto_request(&a, &s, sv[1], 23.5, tb_pair);

    /* A repeated preparation failure exhausts rather than renewing the
     * budget just because the same interface is present. */
    auto_reply(&a, &s, "preparing tb0 address", EXTDISPLAY_SETUP_EV_PROGRESS, 23.5);
    auto_reply(&a, &s, prep_address, EXTDISPLAY_SETUP_EV_ERROR, 33.5);
    CHECK(!strcmp(request(sv[1]), "scan"));
    CHECK(a.suppressed && !strcmp(s.error, prep_address + 6));
    auto_list(&a, &s, tb_up, 1000.0);
    auto_request(&a, &s, sv[1], 1000.0, "");
    CHECK(s.count == 1 && a.suppressed);
    auto_reply(&a, &s, "changed", EXTDISPLAY_SETUP_EV_CHANGED, 1001.0);
    auto_request(&a, &s, sv[1], 1001.0, "scan");
    auto_list(&a, &s, tb_up, 1001.0);
    auto_request(&a, &s, sv[1], 1001.0, "");
    auto_reply(&a, &s, "changed", EXTDISPLAY_SETUP_EV_CHANGED, 1002.0);
    auto_request(&a, &s, sv[1], 1002.0, "scan");
    auto_list(&a, &s, tb_down, 1002.0);
    auto_request(&a, &s, sv[1], 1002.0, "");
    auto_reply(&a, &s, "changed", EXTDISPLAY_SETUP_EV_CHANGED, 1003.0);
    auto_request(&a, &s, sv[1], 1003.0, "scan");
    auto_list(&a, &s, tb_up, 1003.0);
    auto_request(&a, &s, sv[1], 1003.0, tb_pair);

    /* The first post-retry snapshot may already find the same restored link. */
    auto_reply(&a, &s, "error the cable interface disappeared", EXTDISPLAY_SETUP_EV_ERROR, 1004.0);
    auto_request(&a, &s, sv[1], 1004.0, "");
    auto_request(&a, &s, sv[1], 1004.5, "scan");
    auto_list(&a, &s, tb_up, 1004.5);
    auto_request(&a, &s, sv[1], 1004.5, tb_pair);
    auto_reply(&a, &s, "lease tb0 fe80::1 27183", EXTDISPLAY_SETUP_EV_NONE, 1004.5);
    auto_reply(&a, &s, "code ABCD-EFGH", EXTDISPLAY_SETUP_EV_CODE, 1004.5);
    CHECK(s.state == EXTDISPLAY_SETUP_WAITING);
    close(sv[0]);
    close(sv[1]);
}

static void manual_and_auth_refusals(void)
{
    static const struct {
        const char *line;
        enum extdisplay_setup_event ev;
        int manual, cancel;
    } cases[] = {
        { "error the selected interface changed; choose it again", EXTDISPLAY_SETUP_EV_ERROR, 0, 0 },
        { "error the session namespace is not link-local only", EXTDISPLAY_SETUP_EV_ERROR, 0, 0 },
        { "ended handshake-failed", EXTDISPLAY_SETUP_EV_ENDED, 0, 0 },
        { "ended error", EXTDISPLAY_SETUP_EV_ENDED, 0, 0 },
        { "ended carrier-lost", EXTDISPLAY_SETUP_EV_ENDED, 0, 1 }, /* old result crossed Esc */
        { prep_address, EXTDISPLAY_SETUP_EV_ERROR, 1, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int sv[2];
        CHECK(channel(sv) == 0);
        if (failures) return;
        struct extdisplay_setup s;
        struct extdisplay_auto a;
        extdisplay_setup_init(&s, sv[0]);
        extdisplay_auto_init(&a);
        CHECK(extdisplay_setup_watch(&s) == 0);
        CHECK(!strcmp(request(sv[1]), "watch"));
        auto_list(&a, &s, tb_up, 0.0);
        if (cases[i].manual) {
            extdisplay_auto_pause(&a, 0);
            a.selection = s.ifaces[0];
            CHECK(extdisplay_setup_pair(&s, a.selection.name) == 0);
            CHECK(!strcmp(request(sv[1]), tb_pair));
        } else {
            auto_request(&a, &s, sv[1], 0.0, tb_pair);
        }
        if (cases[i].cancel) {
            extdisplay_auto_pause(&a, 1);
            CHECK(extdisplay_setup_cancel(&s) == 0);
            CHECK(!strcmp(request(sv[1]), "cancel"));
        }
        auto_reply(&a, &s, cases[i].line, cases[i].ev, 1.0);
        if (cases[i].cancel) CHECK(!strcmp(s.reason, "cancelled"));
        CHECK(!strcmp(request(sv[1]), "scan"));
        auto_list(&a, &s, tb_up, 1.0);
        auto_request(&a, &s, sv[1], 1000.0, "");
        CHECK(a.suppressed && s.count == 1);
        close(sv[0]);
        close(sv[1]);
    }
}

static void carrier_lost_hotplug(void)
{
    int sv[2];
    CHECK(channel(sv) == 0);
    if (failures) return;
    struct extdisplay_setup s;
    struct extdisplay_auto a;
    extdisplay_setup_init(&s, sv[0]);
    extdisplay_auto_init(&a);

    /* Initial watch -> single candidate -> automatic pair -> paired session. */
    CHECK(extdisplay_setup_watch(&s) == 0);
    CHECK(strcmp(request(sv[1]), "watch") == 0);
    CHECK(extdisplay_setup_line(&s, "iface tb0 thunderbolt-net up 8 02:00:00:00:00:01", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_LIST, 0.0);
    CHECK(extdisplay_auto_step(&a, &s, 0.0) == 1 && s.state == EXTDISPLAY_SETUP_PAIRING);
    CHECK(strcmp(request(sv[1]), "pair tb0 8 02:00:00:00:00:01") == 0);
    CHECK(extdisplay_setup_line(&s, "lease tb0 fe80::1 27183", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "code ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YQ", NULL, 0) == EXTDISPLAY_SETUP_EV_CODE);
    int pair[2];
    CHECK(stream_pair(pair) == 0);
    CHECK(extdisplay_setup_line(&s, "paired", &pair[1], 1) == EXTDISPLAY_SETUP_EV_PAIRED);
    close(s.stream);
    s.stream = -1;
    close(pair[0]);

    /* Cable unplugged mid-session: broker reports carrier-lost. */
    CHECK(extdisplay_setup_line(&s, "ended carrier-lost", NULL, 0) == EXTDISPLAY_SETUP_EV_ENDED);
    CHECK(s.state == EXTDISPLAY_SETUP_LIST);
    CHECK(strstr(extdisplay_setup_reason_text(s.reason), "Waiting for the cable") != NULL);
    extdisplay_auto_after_session(&a, &s);
    CHECK(a.enabled == 1 && a.suppressed == 0 && s.state == EXTDISPLAY_SETUP_SCANNING);
    CHECK(strcmp(request(sv[1]), "scan") == 0);
    /* Empty scan while cable is out: stays waiting, does not close or pair. */
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_LIST, 0.0);
    CHECK(extdisplay_auto_step(&a, &s, 0.0) == 0 && s.state == EXTDISPLAY_SETUP_LIST && s.count == 0);

    /* Cable replugged: broker announces changed -> rescan -> auto-pairs new interface. */
    CHECK(extdisplay_setup_line(&s, "changed", NULL, 0) == EXTDISPLAY_SETUP_EV_CHANGED);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_CHANGED, 0.0);
    CHECK(extdisplay_auto_step(&a, &s, 0.0) == 1 && s.state == EXTDISPLAY_SETUP_SCANNING);
    CHECK(strcmp(request(sv[1]), "scan") == 0);
    CHECK(extdisplay_setup_line(&s, "iface tb1 thunderbolt-net up 11 02:00:00:00:00:01", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_LIST, 0.0);
    CHECK(extdisplay_auto_step(&a, &s, 0.0) == 1 && s.state == EXTDISPLAY_SETUP_PAIRING);
    CHECK(strcmp(request(sv[1]), "pair tb1 11 02:00:00:00:00:01") == 0);

    /* Carrier-lost during waiting also returns to waiting and auto-pairs on return,
     * while ambiguity (two live candidates) still waits for user choice. */
    CHECK(extdisplay_setup_line(&s, "ended carrier-lost", NULL, 0) == EXTDISPLAY_SETUP_EV_ENDED);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_ENDED, 0.0);
    CHECK(a.enabled == 1 && a.suppressed == 0 && s.state == EXTDISPLAY_SETUP_SCANNING);
    CHECK(strcmp(request(sv[1]), "scan") == 0);
    CHECK(extdisplay_setup_line(&s, "iface tb1 thunderbolt-net up 12 02:00:00:00:00:01", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "iface tb2 thunderbolt-net up 13 02:00:00:00:00:02", NULL, 0) == EXTDISPLAY_SETUP_EV_NONE);
    CHECK(extdisplay_setup_line(&s, "end", NULL, 0) == EXTDISPLAY_SETUP_EV_LIST);
    extdisplay_auto_on_event(&a, &s, EXTDISPLAY_SETUP_EV_LIST, 0.0);
    CHECK(extdisplay_auto_step(&a, &s, 0.0) == 0 && s.state == EXTDISPLAY_SETUP_LIST);

    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--control-fd") == 0 && strcmp(argv[2], "3") == 0) return stub();
    flow();
    automatic_flow();
    scan_recovery();
    preparation_recovery();
    manual_and_auth_refusals();
    carrier_lost_hotplug();
    cancelled();
    released();
    errors_and_eof();
    crossing();
    refusals();
    framing();
    spawned(argv[0]);
    printf("%s: extdisplay pairing client against a stub broker\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
