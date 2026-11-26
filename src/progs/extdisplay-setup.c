/*
 * extdisplay's client of the broker's control socket (extdisplay-setup.h). Each broker line
 * is checked against the state it arrives in. Anything unexpected, a line
 * over EXTDISPLAY_SETUP_LINE, or a descriptor where none belongs leaves the channel
 * broken. The pairing code and the address live here only while they are on
 * screen, and are wiped when they leave it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extdisplay-setup.h"
#include "base.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define REPLY_TEXT 200

static int alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
    }
    return 1;
}

/* A kernel interface name: 1 to 15 of [A-Za-z0-9._-]. */
static int ifname_ok(const char *s, size_t n)
{
    if (n < 1 || n > 15) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!(alnum(s[i]) || s[i] == '.' || s[i] == '_' || s[i] == '-')) return 0;
    }
    return 1;
}

/* A link-local address with its zone: hex, ':', '.', '%', and interface-name characters. */
static int linklocal_ok(const char *s, size_t n)
{
    if (n < 1 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!(alnum(s[i]) || s[i] == ':' || s[i] == '.' || s[i] == '%' || s[i] == '-' || s[i] == '_')) return 0;
    }
    return 1;
}

/* Base32 (A-Z, 2-7) in groups separated by single '-', no leading or trailing '-'. */
static int code_ok(const char *s, size_t n)
{
    if (n < 1 || n > 64 || s[0] == '-' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '-') {
            if (s[i - 1] == '-') return 0;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return 0;
    }
    return 1;
}

/* A TCP port in decimal, 1 to 65535, no sign and no leading zero. */
static int port_ok(const char *s, size_t n)
{
    if (n < 1 || n > 5 || s[0] == '0') return 0;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    return v >= 1 && v <= 65535;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* Splits line at single spaces into tok (at most max). -1 on an empty token. */
static int split(char *line, char *tok[], int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return -1;
        char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0) return -1;
        tok[n++] = p;
        if (!e) return n;
        *e = '\0';
        p = e + 1;
    }
}

static enum extdisplay_setup_event broken(struct extdisplay_setup *s, const int *fds, int nfds)
{
    for (int i = 0; i < nfds; i++) close(fds[i]);
    extdisplay_setup_forget(s);
    s->state = EXTDISPLAY_SETUP_BROKEN;
    return EXTDISPLAY_SETUP_EV_BROKEN;
}

static int send_line(struct extdisplay_setup *s, const char *text)
{
    size_t n = strlen(text);
    ssize_t sent = n <= EXTDISPLAY_SETUP_LINE ? send(s->fd, text, n, MSG_NOSIGNAL | MSG_DONTWAIT) : -1;
    if (sent != (ssize_t)n) {
        s->state = EXTDISPLAY_SETUP_BROKEN;
        return -1;
    }
    return 0;
}

void extdisplay_setup_init(struct extdisplay_setup *s, int fd)
{
    memset(s, 0, sizeof *s);
    s->fd = fd;
    s->stream = -1;
    s->state = EXTDISPLAY_SETUP_IDLE;
}

void extdisplay_setup_forget(struct extdisplay_setup *s)
{
    wipe(s->linklocal, sizeof s->linklocal);
    wipe(s->port, sizeof s->port);
    wipe(s->code, sizeof s->code);
}

int extdisplay_setup_scan(struct extdisplay_setup *s)
{
    if (s->state != EXTDISPLAY_SETUP_IDLE && s->state != EXTDISPLAY_SETUP_LIST) return -1;
    memset(s->ifaces, 0, sizeof s->ifaces);
    s->count = 0;
    s->scanned = 0;
    s->changed = 0;
    s->scanning = 1;
    s->state = EXTDISPLAY_SETUP_SCANNING;
    return send_line(s, "scan");
}

int extdisplay_setup_watch(struct extdisplay_setup *s)
{
    if (s->state != EXTDISPLAY_SETUP_IDLE || s->watching) return -1;
    s->watching = 1;
    s->scanning = 1;
    s->state = EXTDISPLAY_SETUP_SCANNING;
    return send_line(s, "watch");
}

int extdisplay_setup_automatic(const struct extdisplay_setup *s)
{
    if (s->state != EXTDISPLAY_SETUP_LIST || s->changed) return -1;
    if (s->count == 1) return 0;
    int choice = -1;
    for (int i = 0; i < s->count; i++) {
        if (!s->ifaces[i].carrier) continue;
        if (choice >= 0) return -1;
        choice = i;
    }
    return choice;
}

int extdisplay_setup_pair(struct extdisplay_setup *s, const char *name)
{
    char line[EXTDISPLAY_SETUP_LINE];
    if (!name || s->state != EXTDISPLAY_SETUP_LIST || !ifname_ok(name, strlen(name))) return -1;
    const struct extdisplay_iface *listed = NULL;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->ifaces[i].name, name) == 0) listed = &s->ifaces[i];
    }
    if (!listed || s->changed) return -1;
    extdisplay_setup_forget(s);
    s->progress[0] = '\0';
    snprintf(s->pairing, sizeof s->pairing, "%s", name);
    if (s->watching && listed->ifindex > 0 && listed->address[0])
        snprintf(line, sizeof line, "pair %s %d %s", name, listed->ifindex, listed->address);
    else snprintf(line, sizeof line, "pair %s", name);
    s->state = EXTDISPLAY_SETUP_PAIRING;
    return send_line(s, line);
}

int extdisplay_setup_cancel(struct extdisplay_setup *s)
{
    /* PAIRED: the session ended here first; the broker goes back to idle. */
    if (s->state != EXTDISPLAY_SETUP_PAIRING && s->state != EXTDISPLAY_SETUP_WAITING && s->state != EXTDISPLAY_SETUP_PAIRED) return -1;
    extdisplay_setup_forget(s);
    s->state = EXTDISPLAY_SETUP_CANCELLING;
    return send_line(s, "cancel");
}

int extdisplay_setup_release(struct extdisplay_setup *s)
{
    if (s->state == EXTDISPLAY_SETUP_BROKEN || s->state == EXTDISPLAY_SETUP_RELEASING || s->state == EXTDISPLAY_SETUP_RELEASED) return -1;
    extdisplay_setup_forget(s);
    s->state = EXTDISPLAY_SETUP_RELEASING;
    return send_line(s, "release");
}

enum extdisplay_setup_event extdisplay_setup_line(struct extdisplay_setup *s, const char *text, const int *fds, int nfds)
{
    char line[EXTDISPLAY_SETUP_LINE + 1], *tok[8];
    size_t n = text ? strlen(text) : 0;
    if (s->state == EXTDISPLAY_SETUP_BROKEN || s->state == EXTDISPLAY_SETUP_RELEASED || !text || n == 0 ||
        n > EXTDISPLAY_SETUP_LINE || !printable(text, n)) {
        return broken(s, fds, nfds);
    }
    /* "error TEXT" carries spaces in its text: taken before the split. */
    if (strncmp(text, "error ", 6) == 0) {
        if ((s->state != EXTDISPLAY_SETUP_PAIRING && s->state != EXTDISPLAY_SETUP_CANCELLING &&
             s->state != EXTDISPLAY_SETUP_SCANNING && s->state != EXTDISPLAY_SETUP_RELEASING) || nfds || n == 6) {
            return broken(s, fds, nfds);
        }
        /* A scan refused ("error busy" while a lease is active): shown, and
         * the (empty) list stays, for F5 later. */
        if (s->state == EXTDISPLAY_SETUP_SCANNING) s->scanned = 1;
        s->scanning = 0;
        extdisplay_setup_forget(s);
        s->reason[0] = '\0';
        s->error[0] = '\0';
        if (s->state == EXTDISPLAY_SETUP_CANCELLING) {
            /* The pair was refused as the cancel went out: that ends the
             * attempt, and nothing else follows. */
            snprintf(s->reason, sizeof s->reason, "%s", "cancelled");
            s->state = s->scanned ? EXTDISPLAY_SETUP_LIST : EXTDISPLAY_SETUP_IDLE;
            return EXTDISPLAY_SETUP_EV_ENDED;
        }
        snprintf(s->error, sizeof s->error, "%.*s", REPLY_TEXT, text + 6);
        if (s->state == EXTDISPLAY_SETUP_RELEASING) return EXTDISPLAY_SETUP_EV_NONE;
        s->state = s->scanned ? EXTDISPLAY_SETUP_LIST : EXTDISPLAY_SETUP_IDLE;
        return EXTDISPLAY_SETUP_EV_ERROR;
    }
    memcpy(line, text, n + 1);
    int ntok = split(line, tok, 8);
    if (ntok < 1) return broken(s, fds, nfds);

    if (strcmp(tok[0], "paired") == 0) {
        if (ntok != 1 || nfds != 1) return broken(s, fds, nfds);
        int stream = fds[0];
        int type = 0;
        socklen_t size = sizeof type;
        struct sockaddr_storage addr;
        socklen_t length = sizeof addr;
        int plain = getsockopt(stream, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_STREAM &&
                    getsockname(stream, (struct sockaddr *)&addr, &length) == 0 && addr.ss_family == AF_UNIX;
        if (!plain) return broken(s, fds, nfds);
        if (s->state == EXTDISPLAY_SETUP_CANCELLING || s->state == EXTDISPLAY_SETUP_RELEASING) {
            /* The sender paired as cancel/release went out: refuse the stream. */
            close(stream);
            return EXTDISPLAY_SETUP_EV_NONE;
        }
        if (s->state != EXTDISPLAY_SETUP_WAITING) return broken(s, fds, nfds);
        fcntl(stream, F_SETFD, FD_CLOEXEC);
        extdisplay_setup_forget(s);
        s->stream = stream;
        s->state = EXTDISPLAY_SETUP_PAIRED;
        return EXTDISPLAY_SETUP_EV_PAIRED;
    }
    if (nfds) return broken(s, fds, nfds);

    if (strcmp(tok[0], "changed") == 0) {
        if (ntok != 1 || !s->watching) return broken(s, fds, nfds);
        s->changed = 1;
        return EXTDISPLAY_SETUP_EV_CHANGED;
    }
    if (strcmp(tok[0], "preparing") == 0) {
        int crossing = s->state == EXTDISPLAY_SETUP_CANCELLING || s->state == EXTDISPLAY_SETUP_RELEASING;
        if (ntok != 3 || (s->state != EXTDISPLAY_SETUP_PAIRING && !crossing) || strcmp(tok[1], s->pairing) ||
            (strcmp(tok[2], "carrier") && strcmp(tok[2], "address") && strcmp(tok[2], "discovery") && strcmp(tok[2], "listener")))
            return broken(s, fds, nfds);
        if (crossing) return EXTDISPLAY_SETUP_EV_NONE;
        snprintf(s->progress, sizeof s->progress, "%s", tok[2]);
        return EXTDISPLAY_SETUP_EV_PROGRESS;
    }
    if (strcmp(tok[0], "iface") == 0) {
        int releasing = s->state == EXTDISPLAY_SETUP_RELEASING && s->scanning;
        if ((s->state != EXTDISPLAY_SETUP_SCANNING && !releasing) || ntok != (s->watching ? 6 : 4)) return broken(s, fds, nfds);
        size_t driver = strlen(tok[2]);
        if (!ifname_ok(tok[1], strlen(tok[1])) || driver < 1 || driver > 32) return broken(s, fds, nfds);
        int up = strcmp(tok[3], "up") == 0, down = strcmp(tok[3], "down") == 0;
        if (!up && !down) return broken(s, fds, nfds);
        int ifindex = 0;
        if (s->watching) {
            char *end;
            long value = strtol(tok[4], &end, 10);
            size_t length = strlen(tok[5]);
            if (tok[4][0] < '1' || tok[4][0] > '9' || *end || value < 1 || value > INT_MAX ||
                length < 2 || length >= sizeof s->ifaces[0].address) return broken(s, fds, nfds);
            for (size_t j = 0; j < length; j++) {
                char c = tok[5][j];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':')) return broken(s, fds, nfds);
            }
            ifindex = (int)value;
        }
        for (int j = 0; j < s->count; j++)
            if (!strcmp(s->ifaces[j].name, tok[1])) return broken(s, fds, nfds);
        if (s->count < EXTDISPLAY_SETUP_IFACES) {
            struct extdisplay_iface *i = &s->ifaces[s->count++];
            snprintf(i->name, sizeof i->name, "%s", tok[1]);
            snprintf(i->driver, sizeof i->driver, "%s", tok[2]);
            i->carrier = up;
            i->ifindex = ifindex;
            if (s->watching) snprintf(i->address, sizeof i->address, "%s", tok[5]);
        }
        return EXTDISPLAY_SETUP_EV_NONE;
    }
    if (strcmp(tok[0], "end") == 0) {
        int releasing = s->state == EXTDISPLAY_SETUP_RELEASING && s->scanning;
        if ((s->state != EXTDISPLAY_SETUP_SCANNING && !releasing) || ntok != 1) return broken(s, fds, nfds);
        s->scanned = 1;
        s->scanning = 0;
        if (s->state != EXTDISPLAY_SETUP_RELEASING) s->state = EXTDISPLAY_SETUP_LIST;
        return releasing ? EXTDISPLAY_SETUP_EV_NONE : EXTDISPLAY_SETUP_EV_LIST;
    }
    /* The broker answers "pair" before it reads a cancel sent meanwhile: in
     * CANCELLING its lease and code still come, checked and not kept. */
    int crossing = s->state == EXTDISPLAY_SETUP_CANCELLING || s->state == EXTDISPLAY_SETUP_RELEASING;
    if (strcmp(tok[0], "lease") == 0) {
        if ((s->state != EXTDISPLAY_SETUP_PAIRING && !crossing) || ntok != 4 || strcmp(tok[1], s->pairing) != 0 ||
            (!crossing && s->linklocal[0]) || !linklocal_ok(tok[2], strlen(tok[2])) ||
            !port_ok(tok[3], strlen(tok[3]))) {
            return broken(s, fds, nfds);
        }
        if (crossing) return EXTDISPLAY_SETUP_EV_NONE;
        snprintf(s->linklocal, sizeof s->linklocal, "%s", tok[2]);
        snprintf(s->port, sizeof s->port, "%s", tok[3]);
        return EXTDISPLAY_SETUP_EV_NONE;
    }
    if (strcmp(tok[0], "code") == 0) {
        if ((s->state != EXTDISPLAY_SETUP_PAIRING && !crossing) || ntok != 2 || (!crossing && !s->linklocal[0]) ||
            !code_ok(tok[1], strlen(tok[1]))) {
            return broken(s, fds, nfds);
        }
        if (crossing) return EXTDISPLAY_SETUP_EV_NONE;
        snprintf(s->code, sizeof s->code, "%s", tok[1]);
        s->state = EXTDISPLAY_SETUP_WAITING;
        return EXTDISPLAY_SETUP_EV_CODE;
    }
    if (strcmp(tok[0], "ended") == 0) {
        static const char *const reasons[] = { "handshake-failed", "timeout", "carrier-lost", "released",
                                               "cancelled", "error" };
        if (ntok != 2) return broken(s, fds, nfds);
        int known = 0;
        for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
            if (strcmp(tok[1], reasons[i]) == 0) known = 1;
        }
        if (!known) return broken(s, fds, nfds);
        /* A late answer to a cancel crossing an unsolicited end: the scan
         * that followed goes on. */
        if (s->state == EXTDISPLAY_SETUP_SCANNING && strcmp(tok[1], "released") != 0) return EXTDISPLAY_SETUP_EV_NONE;
        snprintf(s->reason, sizeof s->reason, "%s", tok[1]);
        extdisplay_setup_forget(s);
        if (strcmp(tok[1], "released") == 0) {
            s->state = EXTDISPLAY_SETUP_RELEASED; /* the broker exits; its end of the channel closes */
        } else {
            s->state = s->scanned ? EXTDISPLAY_SETUP_LIST : EXTDISPLAY_SETUP_IDLE;
        }
        return EXTDISPLAY_SETUP_EV_ENDED;
    }
    return broken(s, fds, nfds);
}

enum extdisplay_setup_event extdisplay_setup_read(struct extdisplay_setup *s)
{
    char buf[EXTDISPLAY_SETUP_LINE + 1];
    union { struct cmsghdr align; char bytes[CMSG_SPACE(4 * sizeof(int))]; } ctl;
    struct iovec iov = { buf, sizeof buf };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.bytes,
                          .msg_controllen = sizeof ctl.bytes };
    int fds[4], nfds = 0, total = 0;
    ssize_t n;
    if (s->state == EXTDISPLAY_SETUP_BROKEN) return EXTDISPLAY_SETUP_EV_BROKEN;
    do n = recvmsg(s->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return EXTDISPLAY_SETUP_EV_NONE;
        s->state = EXTDISPLAY_SETUP_BROKEN;
        return EXTDISPLAY_SETUP_EV_BROKEN;
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int f;
            memcpy(&f, (char *)CMSG_DATA(h) + i * sizeof f, sizeof f);
            if (total < 4) fds[nfds++] = f;
            else close(f);
            total++;
        }
    }
    if (n == 0) { /* the broker closed its end: released, or gone */
        for (int i = 0; i < nfds; i++) close(fds[i]);
        s->state = s->state == EXTDISPLAY_SETUP_RELEASING || s->state == EXTDISPLAY_SETUP_RELEASED ? EXTDISPLAY_SETUP_RELEASED
                                                                                      : EXTDISPLAY_SETUP_BROKEN;
        return s->state == EXTDISPLAY_SETUP_RELEASED ? EXTDISPLAY_SETUP_EV_ENDED : EXTDISPLAY_SETUP_EV_BROKEN;
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n > (ssize_t)EXTDISPLAY_SETUP_LINE) {
        for (int i = 0; i < nfds; i++) close(fds[i]);
        s->state = EXTDISPLAY_SETUP_BROKEN;
        return EXTDISPLAY_SETUP_EV_BROKEN;
    }
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return extdisplay_setup_line(s, buf, fds, nfds);
}

const char *extdisplay_setup_reason_text(const char *reason)
{
    if (!reason) return N_("The broker ended the pairing.");
    if (strcmp(reason, "handshake-failed") == 0) return N_("The sender failed the handshake. Check the code and try again.");
    if (strcmp(reason, "timeout") == 0) return N_("The sender did not answer in time.");
    if (strcmp(reason, "carrier-lost") == 0) return N_("The cable was unplugged. Waiting for the cable.");
    if (strcmp(reason, "released") == 0) return N_("Disconnected.");
    if (strcmp(reason, "cancelled") == 0) return N_("Pairing cancelled.");
    if (strcmp(reason, "error") == 0) return N_("The connection ended, or the broker failed.");
    return N_("The broker ended the pairing.");
}

static int same_iface(const struct extdisplay_iface *a, const struct extdisplay_iface *b)
{
    return a->ifindex == b->ifindex && strcmp(a->address, b->address) == 0;
}

void extdisplay_auto_init(struct extdisplay_auto *a)
{
    memset(a, 0, sizeof *a);
    a->enabled = 1;
}

void extdisplay_auto_on_event(struct extdisplay_auto *a, struct extdisplay_setup *s, enum extdisplay_setup_event ev)
{
    switch (ev) {
    case EXTDISPLAY_SETUP_EV_CHANGED:
        if (s->state == EXTDISPLAY_SETUP_LIST || s->state == EXTDISPLAY_SETUP_SCANNING) a->enabled = 1;
        break;
    case EXTDISPLAY_SETUP_EV_LIST: {
        int blocked_present = 0;
        for (int i = 0; i < s->count; i++) {
            if (same_iface(&a->selection, &s->ifaces[i])) a->selected = i;
            if (same_iface(&a->suppressed_iface, &s->ifaces[i])) {
                blocked_present = 1;
                if (s->ifaces[i].carrier != a->suppressed_iface.carrier) {
                    a->suppressed_iface.carrier = s->ifaces[i].carrier;
                    if (a->suppressed_iface.carrier) a->suppressed = 0;
                }
            }
        }
        if (!blocked_present) a->suppressed = 0;
        break;
    }
    case EXTDISPLAY_SETUP_EV_ERROR:
        a->enabled = 0;
        a->suppressed = 1;
        a->suppressed_iface = a->selection;
        if (s->state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(s);
        break;
    case EXTDISPLAY_SETUP_EV_ENDED:
        if (strcmp(s->reason, "cancelled") != 0) a->suppressed = 0;
        a->enabled = !a->suppressed;
        a->suppressed_iface = a->selection;
        if (s->state == EXTDISPLAY_SETUP_IDLE || s->state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(s);
        break;
    default:
        break;
    }
    if (s->state == EXTDISPLAY_SETUP_LIST && s->count && a->selected >= s->count) a->selected = 0;
}

void extdisplay_auto_after_session(struct extdisplay_auto *a, struct extdisplay_setup *s)
{
    a->enabled = 1;
    a->suppressed = 0;
    a->suppressed_iface = a->selection;
    if (s->state == EXTDISPLAY_SETUP_IDLE || s->state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(s);
}

int extdisplay_auto_step(struct extdisplay_auto *a, struct extdisplay_setup *s)
{
    if (s->state != EXTDISPLAY_SETUP_LIST) return 0;
    if (s->changed) {
        extdisplay_setup_scan(s);
        return 1;
    }
    if (a->enabled && !a->suppressed) {
        int choice = extdisplay_setup_automatic(s);
        if (choice >= 0) {
            a->selected = choice;
            a->selection = s->ifaces[choice];
            s->error[0] = s->reason[0] = '\0';
            a->enabled = 0;
            extdisplay_setup_pair(s, a->selection.name);
            return 1;
        }
    }
    return 0;
}

/* The broker's end of the channel is fd 3; everything else the child holds is
 * closed before it runs (the session owner's descriptor included). */
pid_t extdisplay_setup_spawn_broker(const char *path, int *control)
{
    int sv[2];
    if (!path || !control) {
        errno = EINVAL;
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        int ctl = fcntl(sv[1], F_DUPFD, 10), null = open("/dev/null", O_RDWR);
        if (ctl < 0 || null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0 ||
            close_range(3, ctl - 1, 0) < 0 || close_range(ctl + 1, ~0u, 0) < 0 ||
            dup2(ctl, EXTDISPLAY_BROKER_FD) < 0 || close(ctl) < 0) {
            _exit(127);
        }
        sigset_t none;
        sigemptyset(&none);
        if (sigprocmask(SIG_SETMASK, &none, NULL) < 0) _exit(127);
        char *const argv[] = { "extdisplay-broker", "--control-fd", "3", NULL };
        char *const env[] = { "LANG=C", "PATH=/usr/bin", NULL };
        execve(path, argv, env);
        _exit(127); /* the channel then ends at once: the frontend shows that */
    }
    close(sv[1]);
    *control = sv[0];
    return pid;
}

void extdisplay_setup_reap_broker(pid_t pid)
{
    if (pid <= 0) return;
    /* 5 s: returning the device to NetworkManager polls for up to 2 s. */
    for (int i = 0; i < 500; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        poll(NULL, 0, 10);
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        poll(NULL, 0, 10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
