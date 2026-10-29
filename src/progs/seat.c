/*
 * seat: bridge between LOCachy's process 1 switcher and Wayland compositors
 * (cage, labwc) using the seatd protocol (libseat backend).
 *
 * It links base.h so process 1 treats it as a normal payload (frozen on
 * Ctrl+Alt+Esc, memory pushed to swap, restored on switch back). It runs a
 * private seatd server on $SEATD_SOCK, has udevd (udev-client) describe the
 * input and display devices for libinput, launches the compositor, and
 * coordinates DRM master and seat transitions.
 *
 * With labwc, the shared compositor, it also starts its host clients:
 * labwc-control (the containers' display locachy-0, window control, screens),
 * locachy-bar (status bar, input method) and swaybg (the wallpaper). Process 1 is told
 * "display-ready" once both wayland-0 and locachy-0 are there, and the shared
 * Xwayland's display (XWAYLAND.md) or its time is up. Its gestures
 * (/usr/local/lib/locachy/gestures) run while it is on the screen.
 */
#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base.h"
#include "internal.h"

#define CLIENT_EVENT(opcode) (opcode)
#define SERVER_EVENT(opcode) ((opcode) + (1 << 15))

#define CLIENT_OPEN_SEAT      CLIENT_EVENT(1)
#define CLIENT_CLOSE_SEAT     CLIENT_EVENT(2)
#define CLIENT_OPEN_DEVICE    CLIENT_EVENT(3)
#define CLIENT_CLOSE_DEVICE   CLIENT_EVENT(4)
#define CLIENT_DISABLE_SEAT   CLIENT_EVENT(5)
#define CLIENT_SWITCH_SESSION CLIENT_EVENT(6)
#define CLIENT_PING           CLIENT_EVENT(7)

#define SERVER_SEAT_OPENED      SERVER_EVENT(1)
#define SERVER_SEAT_CLOSED      SERVER_EVENT(2)
#define SERVER_DEVICE_OPENED    SERVER_EVENT(3)
#define SERVER_DEVICE_CLOSED    SERVER_EVENT(4)
#define SERVER_DISABLE_SEAT     SERVER_EVENT(5)
#define SERVER_ENABLE_SEAT      SERVER_EVENT(6)
#define SERVER_PONG             SERVER_EVENT(7)
#define SERVER_SESSION_SWITCHED SERVER_EVENT(8)
#define SERVER_SEAT_DISABLED    SERVER_EVENT(9)
#define SERVER_ERROR            SERVER_EVENT(0x7FFF)

struct proto_header {
    uint16_t opcode;
    uint16_t size;
} __attribute__((packed));

struct proto_server_device_opened {
    int device_id;
} __attribute__((packed));

struct proto_server_error {
    int error_code;
} __attribute__((packed));

struct seat_dev {
    int id;
    int fd;
    char path[128];
    int is_drm;
};

static struct seat_dev devs[64];
static int ndevs, next_dev_id = 1;
static int active_seat = 1;
static int disable_ack_received;

/* The input and display devices described by udevd (udev-client) before the
 * compositor looks for them: libinput takes only the devices udevd has
 * described. Then the pointer settings of rc.conf. */
static void describe_devices(void)
{
    for (int i = 0; i < 100 && access("/run/udev/control", F_OK) != 0; i++) {
        usleep(100000);
    }
    if (access("/run/udev/control", F_OK) != 0) {
        say("seat: no udevd (service_udev): the compositor will find no keyboard or pointer");
    } else if (system("timeout 30 /usr/bin/udevadm trigger --action=add --subsystem-match=input "
                      "--subsystem-match=drm --settle") != 0) {
        say("seat: udevadm trigger failed");
    }
    if (system("/usr/local/bin/settings --apply 2>/dev/null || true") < 0) {
    }
}

static int send_fd(int sock, uint16_t opcode, const void *data, size_t len, int fd)
{
    struct proto_header hdr = { .opcode = opcode, .size = (uint16_t)len };
    struct iovec iov[2] = {
        { .iov_base = &hdr, .iov_len = sizeof hdr },
        { .iov_base = (void *)data, .iov_len = len }
    };
    char cmsg_buf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = len > 0 ? 2 : 1,
    };
    if (fd >= 0) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof cmsg_buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(sock, &msg, MSG_NOSIGNAL) > 0 ? 0 : -1;
}

static int send_msg(int sock, uint16_t opcode, const void *data, size_t len)
{
    return send_fd(sock, opcode, data, len, -1);
}

static void send_err(int sock, int err)
{
    struct proto_server_error e = { .error_code = err };
    send_msg(sock, SERVER_ERROR, &e, sizeof e);
}

static int request_master(int ctl)
{
    if (ctl < 0) return 0;
    if (control_send(ctl, "master", 6, NULL, 0) < 0) return -1;
    struct pollfd p = { .fd = ctl, .events = POLLIN };
    if (poll(&p, 1, 400) > 0 && (p.revents & POLLIN)) {
        char msg[CONTROL_MAX];
        int fds[CONTROL_MAX_FDS], nfds = 0;
        int len = control_recv(ctl, msg, sizeof msg, fds, &nfds);
        for (int i = 0; i < nfds; i++) close(fds[i]);
        if (len == 8 && strcmp(msg, "mastered") == 0) return 0;
    }
    say("seat: process 1 did not hand over DRM master");
    return -1;
}

static int drop_all_devices(void)
{
    double deadline = uptime() + 0.10;
    for (int i = 0; i < ndevs; i++) {
        if (!devs[i].is_drm) continue;
        while (ioctl(devs[i].fd, DRM_IOCTL_DROP_MASTER, 0) < 0) {
            if (errno == EINVAL || errno == EACCES) break; /* already not master */
            if (uptime() >= deadline) {
                say("seat: cannot drop DRM master on %s: %m", devs[i].path);
                return -1;
            }
            usleep(1000);
        }
    }
    return 0;
}

static int acquire_drm_masters(void)
{
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].is_drm && ioctl(devs[i].fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
            say("seat: cannot acquire DRM master on %s: %m", devs[i].path);
            drop_all_devices();
            return -1;
        }
    }
    return 0;
}

static void handle_client_msg(int cfd)
{
    struct proto_header hdr;
    ssize_t n = recv(cfd, &hdr, sizeof hdr, MSG_DONTWAIT);
    if (n <= 0) {
        return;
    }
    uint8_t payload[512];
    if (hdr.size > sizeof payload) {
        return;
    }
    if (n != sizeof hdr || hdr.size > sizeof payload) {
        send_err(cfd, EPROTO);
        return;
    }
    if (hdr.size > 0 && recv(cfd, payload, hdr.size, MSG_WAITALL) != hdr.size) {
        send_err(cfd, EPROTO);
        return;
    }

    switch (hdr.opcode) {
    case CLIENT_OPEN_SEAT: {
        const char *seat_name = "seat0";
        uint16_t slen = (uint16_t)(strlen(seat_name) + 1);
        char resp[64];
        memcpy(resp, &slen, 2);
        memcpy(resp + 2, seat_name, slen);
        send_msg(cfd, SERVER_SEAT_OPENED, resp, 2 + slen);
        if (active_seat) {
            send_msg(cfd, SERVER_ENABLE_SEAT, NULL, 0);
        }
        break;
    }
    case CLIENT_OPEN_DEVICE: {
        uint16_t plen;
        if (hdr.size < sizeof plen || ndevs == 64) {
            send_err(cfd, EINVAL);
            break;
        }
        memcpy(&plen, payload, sizeof plen);
        if (plen == 0 || plen > hdr.size - sizeof plen || plen >= 128) {
            send_err(cfd, EINVAL);
            break;
        }
        char path[128];
        snprintf(path, sizeof path, "%.*s", (int)plen, (char *)payload + 2);
        if (strncmp(path, "/dev/dri/", 9) != 0 && strncmp(path, "/dev/input/event", 16) != 0) {
            send_err(cfd, EACCES);
            break;
        }
        int dev_fd = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (dev_fd < 0) {
            send_err(cfd, errno);
        } else {
            int is_drm = (strstr(path, "/dri/card") != NULL);
            if (is_drm && active_seat && ioctl(dev_fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
                int error = errno;
                close(dev_fd);
                send_err(cfd, error);
                break;
            }
            int id = next_dev_id++;
            if (ndevs < 64) {
                devs[ndevs].id = id;
                devs[ndevs].fd = dev_fd;
                snprintf(devs[ndevs].path, sizeof devs[ndevs].path, "%s", path);
                devs[ndevs].is_drm = is_drm;
                ndevs++;
            }
            struct proto_server_device_opened resp = { .device_id = id };
            send_fd(cfd, SERVER_DEVICE_OPENED, &resp, sizeof resp, dev_fd);
        }
        break;
    }
    case CLIENT_CLOSE_DEVICE: {
        int id;
        if (hdr.size != sizeof id) {
            send_err(cfd, EINVAL);
            break;
        }
        memcpy(&id, payload, sizeof id);
        for (int i = 0; i < ndevs; i++) {
            if (devs[i].id == id) {
                if (devs[i].is_drm) {
                    ioctl(devs[i].fd, DRM_IOCTL_DROP_MASTER, 0);
                }
                close(devs[i].fd);
                devs[i] = devs[--ndevs];
                break;
            }
        }
        send_msg(cfd, SERVER_DEVICE_CLOSED, NULL, 0);
        break;
    }
    case CLIENT_DISABLE_SEAT: {
        disable_ack_received = 1;
        send_msg(cfd, SERVER_SEAT_DISABLED, NULL, 0);
        break;
    }
    case CLIENT_PING: {
        send_msg(cfd, SERVER_PONG, NULL, 0);
        break;
    }
    case CLIENT_CLOSE_SEAT: {
        send_msg(cfd, SERVER_SEAT_CLOSED, NULL, 0);
        break;
    }
    default:
        send_err(cfd, EINVAL);
        break;
    }
}

/* A host client of the compositor; it waits for wayland-0 itself. */
static pid_t start_client(const char *path)
{
    if (access(path, X_OK) != 0) {
        say("seat: %s is not installed", path);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* The gestures (libinput-gestures, lisgd: link/gestures), in a process
 * group of their own: started (on 1) when the shared display is on the
 * screen, ended when it goes, so that no swipe meant for a direct program
 * reaches them. */
#define GESTURES "/usr/local/lib/locachy/gestures"
static pid_t gestures_pid = -1;

static void gestures(int on)
{
    if (gestures_pid > 0) {
        kill(-gestures_pid, SIGTERM);
        waitpid(gestures_pid, NULL, 0);
        gestures_pid = -1;
    }
    if (!on || access(GESTURES, X_OK) != 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        execl(GESTURES, GESTURES, (char *)NULL);
        _exit(127);
    }
    gestures_pid = pid;
}

/* labwc's Xwayland: its xinitrc (link/labwc-xinitrc) writes the display the
 * containers get, empty when X11 is unavailable. display-ready waits for it
 * (when Xwayland is installed) up to X11_WAIT seconds, so that the first
 * program started on it finds X11 there; Wayland comes without it. */
#define X11_RECORD "/run/user/0/locachy-x11-display"
#define XWAYLAND "/usr/bin/Xwayland"
#define X11_WAIT 10

/* The wallpaper, rc.conf's wallpaper (settings, or w in files): swaybg
 * with it, once wayland-0 is there, and started again when it has changed or
 * swaybg has exited. It is changed while the shared display is frozen, so it
 * is looked at when the seat comes back. */
static pid_t bg_pid = -1;
static char bg_path[512];

static void wallpaper_setting(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "wallpaper") == 0) {
        snprintf(arg, sizeof bg_path, "%s", value);
    }
}

static void wallpaper(void)
{
    char path[sizeof bg_path] = "";
    int status;
    rc_read(wallpaper_setting, path);
    if (bg_pid > 0 && waitpid(bg_pid, &status, WNOHANG) == bg_pid) {
        say("seat: swaybg exited with status %d; starting it again",
            WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        bg_pid = -1;
    }
    if (bg_pid > 0 && strcmp(path, bg_path) == 0) {
        return;
    }
    if (bg_pid > 0) {
        kill(bg_pid, SIGTERM);
        waitpid(bg_pid, NULL, 0);
        bg_pid = -1;
    }
    memcpy(bg_path, path, sizeof bg_path);
    if (!path[0]) {
        return;
    }
    if (access(path, R_OK) != 0) {
        say("seat: wallpaper %s: %m", path);
        return;
    }
    bg_pid = fork();
    if (bg_pid == 0) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        execl("/usr/bin/swaybg", "swaybg", "-m", "fill", "-i", path, (char *)NULL);
        _exit(127);
    }
    say("seat: wallpaper %s", path);
}

/* From locachy-bar, which holds the keyboard in the input method's modes so
 * that process 1 does not see the keys: SIGUSR1 key_home, SIGUSR2 key_lock
 * (and labwc-control: idle); from labwc-control's touchpad swipes, SIGRTMIN
 * the next task, SIGRTMIN + 1 the previous. Passed on to process 1. */
static volatile sig_atomic_t want_home, want_lock, want_switch;

static void on_sigswitch(int sig)
{
    want_switch = sig == SIGRTMIN ? 1 : -1;
}

static void on_sigusr1(int sig)
{
    (void)sig;
    want_home = 1;
}

static void on_sigusr2(int sig)
{
    (void)sig;
    want_lock = 1;
}

static void on_rc_keymap(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "keymap") == 0 && value && value[0]) {
        snprintf((char *)arg, 32, "%s", value);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: seat COMMAND [ARG...]\n");
        return 2;
    }
    signal(SIGUSR1, on_sigusr1);
    signal(SIGUSR2, on_sigusr2);
    signal(SIGRTMIN, on_sigswitch);
    signal(SIGRTMIN + 1, on_sigswitch);

    int ctl = control_fd();
    if (ctl >= 0) {
        while (ctl >= 0) {
            char msg[CONTROL_MAX];
            int fds[CONTROL_MAX_FDS], nfds = 0;
            int len = control_recv(ctl, msg, sizeof msg, fds, &nfds);
            for (int i = 0; i < nfds; i++) {
                close(fds[i]);
            }
            if (len <= 0) {
                return 1;
            }
            if (strcmp(msg, "take") == 0) {
                if (request_master(ctl) < 0) return 1;
                break;
            }
        }
    }

    describe_devices();

    /* In the runtime directory process 1 made (XDG_RUNTIME_DIR). */
    const char *sock_path = "/run/user/0/seatd.sock";
    unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un un = { .sun_family = AF_UNIX };
    snprintf(un.sun_path, sizeof un.sun_path, "%s", sock_path);
    if (bind(lfd, (struct sockaddr *)&un, sizeof un) < 0 || listen(lfd, 1) < 0) {
        say("seat: cannot bind %s: %s", sock_path, strerror(errno));
        return 1;
    }
    chmod(sock_path, 0600);

    int is_labwc = strcmp(argv[1], "/usr/bin/labwc") == 0;
    if (is_labwc) {
        /* Left over when the last session was killed: never offer them. */
        unlink("/run/user/0/locachy-0");
        unlink("/run/user/0/locachy-control.sock");
        unlink("/run/user/0/locachy-bar.sock");
        unlink(X11_RECORD);
    }

    pid_t comp_pid = fork();
    if (comp_pid == 0) {
        /* The layout of the key table (keytable: rc.conf's keymap, through
         * systemd's kbd-model-map, as localectl pairs them), as the direct
         * programs type; without a table, rc.conf's keymap as a layout name. */
        static const char *const xkb[][2] = { { "layout", "XKB_DEFAULT_LAYOUT" }, { "model", "XKB_DEFAULT_MODEL" },
                                              { "variant", "XKB_DEFAULT_VARIANT" }, { "options", "XKB_DEFAULT_OPTIONS" } };
        if (keys_layout_name("layout")) {
            for (size_t i = 0; i < sizeof xkb / sizeof xkb[0]; i++) {
                const char *value = keys_layout_name(xkb[i][0]);
                if (value) setenv(xkb[i][1], value, 1);
            }
        } else {
            char kmap[32] = "";
            rc_read(on_rc_keymap, kmap);
            if (kmap[0]) {
                setenv("XKB_DEFAULT_LAYOUT", kmap, 1);
            }
        }
        setenv("LIBSEAT_BACKEND", "seatd", 1);
        setenv("SEATD_SOCK", sock_path, 1);
        if (is_labwc) {
            /* Its Xwayland's display is labwc's to choose (DISPLAY), none other. */
            unsetenv("DISPLAY");
            unsetenv("XAUTHORITY");
            /* settings --apply (above) wrote the configuration with rc.conf's values. */
            const char *rc = access(LABWC_RC, R_OK) == 0 ? LABWC_RC : "/etc/locachy/labwc/rc.xml";
            execl(argv[1], argv[1], "-C", "/etc/locachy/labwc", "-c", rc, (char *)NULL);
        } else {
            execvp(argv[1], argv + 1);
        }
        dprintf(2, "seat: cannot run %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }

    int display_ready_sent = 0;
    double display_started = uptime();
    pid_t bar_pid = -1, control_pid = -1;
    if (is_labwc) {
        control_pid = start_client("/usr/local/lib/locachy/labwc-control");
        bar_pid = start_client("/usr/local/bin/locachy-bar");
    }

    int cfd = -1;
    say("seat: started %s (pid %d), seatd socket %s", argv[1], (int)comp_pid, sock_path);

    for (;;) {
        if (is_labwc && !display_ready_sent && ctl >= 0 &&
            access("/run/user/0/wayland-0", F_OK) == 0 &&
            access("/run/user/0/locachy-0", F_OK) == 0 &&
            (access(X11_RECORD, F_OK) == 0 || access(XWAYLAND, X_OK) != 0 ||
             uptime() - display_started > X11_WAIT)) {
            if (access(X11_RECORD, F_OK) != 0 && access(XWAYLAND, X_OK) == 0) {
                say("seat: Xwayland did not start within %d s: X11 programs cannot open windows", X11_WAIT);
            }
            display_ready_sent = 1;
            control_send(ctl, "display-ready", 13, NULL, 0);
            wallpaper(); /* swaybg needs wayland-0: it does not wait for it */
            gestures(1);
        }
        if (is_labwc && !display_ready_sent && uptime() - display_started > 15) {
            say("seat: labwc startup timed out");
            kill(comp_pid, SIGTERM);
            break;
        }
        if (want_home) {
            want_home = 0;
            if (ctl >= 0) {
                control_send(ctl, "home", 4, NULL, 0);
            }
        }
        if (want_lock) {
            want_lock = 0;
            if (ctl >= 0) {
                control_send(ctl, "lock", 4, NULL, 0);
            }
        }
        if (want_switch) {
            static const char *const next[] = { "switch", "next", NULL }, *const prev[] = { "switch", "prev", NULL };
            int forward = want_switch > 0;
            want_switch = 0;
            if (ctl >= 0) {
                control_send_fields(ctl, forward ? next : prev);
            }
        }
        struct pollfd p[4];
        int n = 0;
        int lfd_i = n; p[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        int cfd_i = -1;
        if (cfd >= 0) {
            cfd_i = n;
            p[n++] = (struct pollfd){ .fd = cfd, .events = POLLIN };
        }
        int ctl_i = -1;
        if (ctl >= 0) {
            ctl_i = n;
            p[n++] = (struct pollfd){ .fd = ctl, .events = POLLIN };
        }

        int r = poll(p, (nfds_t)n, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ctl_i >= 0 && (p[ctl_i].revents & (POLLIN | POLLHUP))) {
            char msg[CONTROL_MAX];
            int fds[CONTROL_MAX_FDS], nfds = 0;
            int len = control_recv(ctl, msg, sizeof msg, fds, &nfds);
            for (int i = 0; i < nfds; i++) {
                close(fds[i]);
            }
            if (len <= 0) {
                break;
            }
            if (strcmp(msg, "take") == 0) {
                if (request_master(ctl) < 0 || acquire_drm_masters() < 0) {
                    say("seat: refusing to enable a compositor without DRM master");
                    kill(comp_pid, SIGTERM);
                    break;
                }
                active_seat = 1;
                if (cfd >= 0) {
                    send_msg(cfd, SERVER_ENABLE_SEAT, NULL, 0);
                }
                if (bar_pid > 0) {
                    kill(bar_pid, SIGUSR1);
                }
                if (is_labwc) {
                    wallpaper();
                    gestures(display_ready_sent);
                }
            } else if (strcmp(msg, "release") == 0) {
                gestures(0);
                if (bar_pid > 0) {
                    kill(bar_pid, SIGUSR2);
                    usleep(10000);
                }
                active_seat = 0;
                disable_ack_received = 0;
                if (cfd >= 0) {
                    send_msg(cfd, SERVER_DISABLE_SEAT, NULL, 0);
                    double deadline = uptime() + 0.25;
                    while (!disable_ack_received && uptime() < deadline) {
                        struct pollfd cp = { .fd = cfd, .events = POLLIN };
                        if (poll(&cp, 1, 10) > 0 && (cp.revents & POLLIN)) {
                            handle_client_msg(cfd);
                        }
                    }
                    if (!disable_ack_received) {
                        say("seat: compositor did not disable its devices; ending it before handover");
                        kill(comp_pid, SIGTERM);
                        break;
                    }
                }
                if (drop_all_devices() < 0) {
                    say("seat: DRM master release failed; ending compositor before retry");
                    kill(comp_pid, SIGTERM);
                    break;
                }
                control_send(ctl, "released", 8, NULL, 0);
            }
        }

        if (p[lfd_i].revents & POLLIN) {
            int new_cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (new_cfd >= 0) {
                if (cfd >= 0) close(cfd);
                cfd = new_cfd;
            }
        }

        if (cfd_i >= 0 && (p[cfd_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (p[cfd_i].revents & POLLIN) {
                handle_client_msg(cfd);
            }
            if (p[cfd_i].revents & (POLLHUP | POLLERR)) {
                close(cfd);
                cfd = -1;
            }
        }

        int st = 0;
        pid_t died = waitpid(comp_pid, &st, WNOHANG);
        if (died > 0) {
            st = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            say("seat: %s (pid %d) exited with status %d", argv[1], (int)died, st);
            break;
        }
    }

    if (bar_pid > 0) {
        kill(bar_pid, SIGTERM);
    }
    if (control_pid > 0) {
        kill(control_pid, SIGTERM);
    }
    if (bg_pid > 0) {
        kill(bg_pid, SIGTERM);
    }
    gestures(0);
    if (is_labwc) {
        unlink("/run/user/0/wayland-0");
        unlink("/run/user/0/wayland-0.lock");
        unlink("/run/user/0/locachy-0");
        unlink("/run/user/0/locachy-control.sock");
        unlink("/run/user/0/locachy-bar.sock");
        unlink(X11_RECORD);
    }
    drop_all_devices();
    if (cfd >= 0) close(cfd);
    close(lfd);
    unlink(sock_path);
    for (int i = 0; i < ndevs; i++) {
        close(devs[i].fd);
    }
    return 0;
}
