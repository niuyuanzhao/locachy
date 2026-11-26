/*
 * extdisplay: extended-display receiver (tools/EXTDISPLAY.md).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base.h"
#include "internal.h"
#include "session.h"
#include "extdisplay-ipc.h"
#include "extdisplay-protocol.h"
#include "extdisplay-setup.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BG           theme_rgb(THEME_BG, 0x101820)
#define PANEL        theme_rgb(THEME_PANEL, 0x203048)
#define FG           theme_rgb(THEME_FG, 0xD0D0D0)
#define ACCENT_TEXT  theme_rgb(THEME_ACCENT_TEXT, 0xD0E8FF)
#define DIM          theme_rgb(THEME_DIM, 0x90A8C0)
#define WARN         theme_rgb(THEME_WARNING, 0xFFB080)
#define HANDSHAKE_S 8.0

static volatile sig_atomic_t stop;
static int ipc = -1, active, outside_cancelled;
static struct extdisplay_setup setup = { .fd = -1, .stream = -1 };
static pid_t broker = -1;
static int leaving, diagnostics;
static struct extdisplay_auto auto_state = { .enabled = 1 };
static uint32_t *frame;
static size_t frame_bytes;
static uint32_t frame_w, frame_h;
static int dmabuf_active;
static int flip_ack_pending;
static double flip_sent_at;
static int scanning_slot = -1, pending_slot = -1;
static uint32_t peer_input;
static struct extdisplay_touch_frame touch_frame;
static int vx, vy, vw, vh, panel_w, panel_h;
static char decode_mode[16] = "auto";
static char extdisplay_card_cfg[64];
static char display_card_cfg[64];
static const char *state = N_("Unavailable");
static const char *detail = N_("Waiting for the sender.");

static void on_rc_config(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (!value) return;
    if (strcmp(name, "extdisplay_decode") == 0 &&
        (strcmp(value, "auto") == 0 || strcmp(value, "hw") == 0 || strcmp(value, "sw") == 0)) {
        snprintf(decode_mode, sizeof decode_mode, "%s", value);
    } else if (strcmp(name, "extdisplay_card") == 0) {
        snprintf(extdisplay_card_cfg, sizeof extdisplay_card_cfg, "%s", value);
    } else if (strcmp(name, "display_card") == 0) {
        snprintf(display_card_cfg, sizeof display_card_cfg, "%s", value);
    }
}

static const char *effective_card_selector(void)
{
    const char *env = getenv("LOCACHY_DRM_CARD");
    if (env && *env) return env;
    if (extdisplay_card_cfg[0] && strcmp(extdisplay_card_cfg, "auto") != 0) return extdisplay_card_cfg;
    if (display_card_cfg[0] && strcmp(display_card_cfg, "auto") != 0) return display_card_cfg;
    return "auto";
}

static void ending(int sig) { (void)sig; stop = 1; }

static void set_state(const char *s, const char *d) { state = s; detail = d; }

static int ipc_send_reliable(struct extdisplay_ipc *m, int timeout_ms)
{
    if (ipc < 0) {
        errno = EBADF;
        return -1;
    }
    if (extdisplay_ipc_send(ipc, m) == 0) return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    double deadline = uptime() + (double)timeout_ms / 1000.0;
    for (;;) {
        int left = (int)((deadline - uptime()) * 1000.0);
        if (left <= 0) {
            errno = EAGAIN;
            return -1;
        }
        struct pollfd p = { ipc, POLLOUT, 0 };
        int r = poll(&p, 1, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            errno = EAGAIN;
            return -1;
        }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
        if (p.revents & POLLOUT) {
            if (extdisplay_ipc_send(ipc, m) == 0) return 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        }
    }
}

static int send_text(uint32_t op, const char *text, int n, int droppable)
{
    if (ipc < 0 || n < 0 || n > (int)EXTDISPLAY_IPC_TEXT) {
        errno = EINVAL;
        return -1;
    }
    struct extdisplay_ipc m = { .op = op, .length = (uint32_t)n };
    if (n) memcpy(m.text, text, (size_t)n);
    if (droppable) {
        if (extdisplay_ipc_send(ipc, &m) == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    /* Only motion and scroll updates may be shed. Losing a transition would
     * leave the sender's input state out of sync until disconnect. */
    return ipc_send_reliable(&m, 250);
}

static int input_allowed(void)
{
    return base_session_fd() >= 0 ? base_session_input_allowed() : control_fd() >= 0;
}

static int send_input(const char *text, int n, int droppable)
{
    if (!input_allowed()) return 0;
    return send_text(EXTDISPLAY_IPC_INPUT, text, n, droppable);
}

static int cancel(void)
{
    char text[96];
    outside_cancelled = 1;
    return active ? send_text(EXTDISPLAY_IPC_INPUT, text, extdisplay_cancel_json(text, sizeof text), 0) : 0;
}

static void viewport(void)
{
    screen_pixels(&panel_w, &panel_h);
    vx = 0;
    vy = 0;
    vw = panel_w > 0 ? panel_w : 1;
    vh = panel_h > 0 ? panel_h : 1;
    if (frame_w && frame_h) {
        if ((uint64_t)vw * frame_h <= (uint64_t)vh * frame_w) vh = (int)((uint64_t)vw * frame_h / frame_w);
        else vw = (int)((uint64_t)vh * frame_w / frame_h);
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
        vx = (panel_w - vw) / 2;
        vy = (panel_h - vh) / 2;
    }
}

static void draw(void)
{
    viewport();
    if (frame && frame_w && frame_h) {
        if (vw == (int)frame_w && vh == (int)frame_h) {
            if (vx > 0) {
                screen_fill_px(0, 0, vx, panel_h, 0x000000);
                screen_fill_px(vx + vw, 0, panel_w - (vx + vw), panel_h, 0x000000);
            }
            if (vy > 0) {
                screen_fill_px(0, 0, panel_w, vy, 0x000000);
                screen_fill_px(0, vy + vh, panel_w, panel_h - (vy + vh), 0x000000);
            }
            screen_blit(vx, vy, vw, vh, frame, vw);
        } else {
            screen_fill_px(0, 0, panel_w, panel_h, 0x000000);
            uint32_t *line = malloc((size_t)vw * sizeof *line);
            for (int y = 0; line && y < vh; y++) {
                const uint32_t *src = frame + (size_t)((uint64_t)y * frame_h / (unsigned)vh) * frame_w;
                for (int x = 0; x < vw; x++) line[x] = src[(uint64_t)x * frame_w / (unsigned)vw];
                screen_blit(vx, vy + y, vw, 1, line, vw);
            }
            free(line);
        }
    } else if (dmabuf_active) {
        return;
    } else {
        screen_fill_px(0, 0, panel_w, panel_h, 0x000000);
    }
    screen_flush();
}

static int touch_frame_flush(void)
{
    char s[1024];
    if (!touch_frame.count) return 0;
    int n = extdisplay_touch_frame_json(s, sizeof s, touch_frame.points, touch_frame.count);
    touch_frame.count = 0;
    return n < 0 ? 0 : send_input(s, n, extdisplay_input_droppable(s, (size_t)n));
}

/* Raw multi-touch and stylus events when negotiated with the peer. */
static int raw_pointer_input(void)
{
    struct pointer_raw_event re;
    char s[256];
    while (pointer_raw_next(&re)) {
        if (!active || vw < 1 || vh < 1) continue;
        /* Same viewport clamp as pointer_input(), keeping sub-pixel precision. */
        double px = re.x < vx ? vx : re.x > vx + vw - 1 ? vx + vw - 1 : re.x;
        double py = re.y < vy ? vy : re.y > vy + vh - 1 ? vy + vh - 1 : re.y;
        double x = vw > 1 ? (px - vx) / (double)(vw - 1) : 0;
        double y = vh > 1 ? (py - vy) / (double)(vh - 1) : 0;
        outside_cancelled = 0;
        int n = -1;
        switch (re.kind) {
        case POINTER_RAW_TOUCH_DOWN:
        case POINTER_RAW_TOUCH_MOVE:
        case POINTER_RAW_TOUCH_UP:
        case POINTER_RAW_TOUCH_CANCEL: {
            enum extdisplay_touch_state st = re.kind == POINTER_RAW_TOUCH_DOWN ? EXTDISPLAY_TOUCH_DOWN :
                re.kind == POINTER_RAW_TOUCH_MOVE ? EXTDISPLAY_TOUCH_MOVE :
                re.kind == POINTER_RAW_TOUCH_UP ? EXTDISPLAY_TOUCH_UP : EXTDISPLAY_TOUCH_CANCEL;
            if (re.slot < 0 || re.slot >= (int)EXTDISPLAY_TOUCH_POINTS_MAX) break;
            if (extdisplay_touch_frame_add(&touch_frame, re.slot, st, x, y, re.xmm, re.ymm) < 0) {
                if (touch_frame_flush() < 0) return -1;
                (void)extdisplay_touch_frame_add(&touch_frame, re.slot, st, x, y, re.xmm, re.ymm);
            }
            break;
        }
        case POINTER_RAW_TOUCH_FRAME:
            if (touch_frame_flush() < 0) return -1;
            break;
        case POINTER_RAW_TABLET_PROXIMITY:
            n = extdisplay_tablet_proximity_json(s, sizeof s, re.proximity_in, (enum extdisplay_tablet_tool)re.tool,
                                                 re.serial ? re.serial : re.tool_id, re.caps, x, y);
            break;
        case POINTER_RAW_TABLET_AXIS:
            n = extdisplay_tablet_axis_json(s, sizeof s, x, y, re.pressure, re.tilt_x, re.tilt_y,
                                            re.distance, re.rotation, re.buttons);
            break;
        case POINTER_RAW_TABLET_TIP:
            n = extdisplay_tablet_tip_json(s, sizeof s, re.tip_down, x, y, re.pressure, re.buttons);
            break;
        case POINTER_RAW_TABLET_BUTTON:
            n = extdisplay_tablet_button_json(s, sizeof s, re.button, re.button_state, x, y, re.buttons);
            break;
        case POINTER_RAW_CANCEL:
            touch_frame.count = 0;
            if (cancel() < 0) return -1;
            break;
        default:
            break;
        }
        if (n >= 0 && send_input(s, n, extdisplay_input_droppable(s, (size_t)n)) < 0) return -1;
    }
    return 0;
}

static int send_pointer_motion(double x, double y, double dx, double dy, int has_delta)
{
    char s[256];
    int n = has_delta ? extdisplay_pointer_delta_json(s, sizeof s, x, y, dx, dy) :
                        extdisplay_pointer_json(s, sizeof s, x, y);
    return n >= 0 ? send_input(s, n, 1) : 0;
}

/* Drain the current pointer_last() (if have_first) plus every remaining queued
 * pointer event in one pass, coalescing consecutive pure motion events so
 * relative dx/dy deltas sum into a single wire packet before any button,
 * scroll, cancel, or raw event. */
static int pointer_drain_input(int have_first)
{
    int have_motion = 0, motion_has_delta = 0;
    double mx = 0, my = 0, mdx = 0, mdy = 0;
    for (;;) {
        if (!have_first && !pointer_next()) break;
        have_first = 0;
        const struct pointer_event *p = pointer_last();
        if (pointer_raw_pending()) {
            if (have_motion) {
                if (send_pointer_motion(mx, my, mdx, mdy, motion_has_delta) < 0) return -1;
                have_motion = motion_has_delta = 0;
                mdx = mdy = 0;
            }
            if (raw_pointer_input() < 0) return -1;
        }
        if (!active || p->gesture == GESTURE_RAW) continue;
        /* Focus loss must clear the peer's held buttons, not commit a click. */
        if (p->cancelled) {
            have_motion = motion_has_delta = 0;
            mdx = mdy = 0;
            if (cancel() < 0) return -1;
            continue;
        }
        if (vw < 1 || vh < 1) {
            have_motion = motion_has_delta = 0;
            mdx = mdy = 0;
            if (!outside_cancelled && cancel() < 0) return -1;
            continue;
        }
        int px = p->x < vx ? vx : p->x >= vx + vw ? vx + vw - 1 : p->x;
        int py = p->y < vy ? vy : p->y >= vy + vh ? vy + vh - 1 : p->y;
        outside_cancelled = 0;
        double x = vw > 1 ? (double)(px - vx) / (vw - 1) : 0;
        double y = vh > 1 ? (double)(py - vy) / (vh - 1) : 0;
        if (!p->button && !p->gesture && !p->wheel && !p->hwheel && !p->scroll_dx && !p->scroll_dy) {
            int is_delta = p->dx != 0 || p->dy != 0;
            if (have_motion && (motion_has_delta != is_delta ||
                                (is_delta && (mdx + p->dx > 16000 || mdx + p->dx < -16000 ||
                                              mdy + p->dy > 16000 || mdy + p->dy < -16000)))) {
                if (send_pointer_motion(mx, my, mdx, mdy, motion_has_delta) < 0) return -1;
                have_motion = motion_has_delta = 0;
                mdx = mdy = 0;
            }
            mx = x;
            my = y;
            if (is_delta) {
                mdx += p->dx;
                mdy += p->dy;
                motion_has_delta = 1;
            }
            have_motion = 1;
            continue;
        }
        if (have_motion) {
            if (send_pointer_motion(mx, my, mdx, mdy, motion_has_delta) < 0) return -1;
            have_motion = motion_has_delta = 0;
            mdx = mdy = 0;
        }
        char s[256];
        int n, droppable = 0;
        if (p->button) {
            n = extdisplay_button_json(s, sizeof s, (uint32_t)p->button, p->pressed, x, y);
        } else if (p->scroll_dx || p->scroll_dy || (p->gesture == GESTURE_SCROLL && p->scroll_source != SCROLL_TOUCH)) {
            double kx = frame_w ? (double)frame_w / vw : 1, ky = frame_h ? (double)frame_h / vh : 1;
            n = extdisplay_scroll_json(s, sizeof s, -p->scroll_dx * kx, -p->scroll_dy * ky, p->scroll_phase, p->scroll_source);
            droppable = p->scroll_phase == SCROLL_UPDATE;
        } else {
            continue;
        }
        if (n >= 0 && send_input(s, n, droppable) < 0) return -1;
    }
    if (have_motion && send_pointer_motion(mx, my, mdx, mdy, motion_has_delta) < 0) return -1;
    if (pointer_raw_pending() && raw_pointer_input() < 0) return -1;
    return 0;
}

/* Every key goes to the sender while the link is up, Super+Esc and Ctrl+Alt+F12
 * included: no key switches away or ends the program; the cable coming out
 * does. Only lock, volume, and brightness bindings stay local. */
static int raw_input(void)
{
    struct base_raw_key e;
    while (base_raw_next(&e)) {
        if (e.state == 3) {
            if (cancel() < 0) return -1;
            continue;
        }
        if (e.flags & BASE_RAW_LOCAL) {
            if (e.state == 1 && cancel() < 0) return -1;
            continue;
        }
        if (!active) continue;
        char text[256];
        int n = extdisplay_key_json(text, sizeof text, e.device, e.code, e.keysym, e.modifiers, e.locks, (int)e.state);
        if (n >= 0 && send_input(text, n, 0) < 0) return -1;
    }
    return 0;
}

static int paired_stream(int fd)
{
    struct sockaddr_storage addr;
    socklen_t size = sizeof addr;
    int type;
    socklen_t type_size = sizeof type;
    return getsockname(fd, (struct sockaddr *)&addr, &size) == 0 && addr.ss_family == AF_UNIX &&
           getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) == 0 && type == SOCK_STREAM;
}

static void end_worker(pid_t worker)
{
    if (worker <= 0 && ipc < 0) return;
    if (ipc >= 0) {
        struct extdisplay_ipc end = { .op = EXTDISPLAY_IPC_STOP };
        extdisplay_ipc_send(ipc, &end);
    }
    for (int i = 0; worker > 0 && i < 20; i++) {
        if (waitpid(worker, NULL, WNOHANG) == worker) { worker = -1; break; }
        poll(NULL, 0, 10);
    }
    if (worker > 0) {
        kill(worker, SIGKILL);
        for (int i = 0; i < 20 && waitpid(worker, NULL, WNOHANG) != worker; i++) poll(NULL, 0, 10);
    }
    if (ipc >= 0) close(ipc);
    ipc = -1;
}

static int release_remote(pid_t *worker, int *inhibited)
{
    if (*inhibited) base_sleep_inhibit(0);
    *inhibited = 0;
    base_raw_enable(0);
    base_raw_enable(1);
    while (pointer_next()) { }
    {
        struct pointer_raw_event discard;
        while (pointer_raw_next(&discard)) { }
        touch_frame.count = 0;
    }
    if (flip_ack_pending && ipc >= 0) {
        if (screen_flip_complete() > 0) {
            int retired = scanning_slot;
            scanning_slot = pending_slot;
            pending_slot = -1;
            flip_ack_pending = 0;
            uint32_t released = retired >= 0 && retired != scanning_slot ? (uint32_t)(retired + 1) : 0u;
            struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = released };
            (void)extdisplay_ipc_send(ipc, &ack);
        } else if (screen_flip_fd() < 0) {
            flip_ack_pending = 0;
            scanning_slot = -1;
            pending_slot = -1;
            struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = EXTDISPLAY_DMABUF_RELEASE_ALL };
            (void)extdisplay_ipc_send(ipc, &ack);
        }
    }
    if (*worker > 0 && ipc >= 0) {
        struct extdisplay_ipc m = { .op = EXTDISPLAY_IPC_CANCEL };
        int confirmed = 0;
        double deadline = uptime() + EXTDISPLAY_CANCEL_WAIT_MS / 1000.0;
        if (extdisplay_ipc_send(ipc, &m) == 0) {
            while (!confirmed) {
                int left = (int)((deadline - uptime()) * 1000);
                struct pollfd p = { ipc, POLLIN, 0 };
                if (left <= 0 || poll(&p, 1, left) <= 0) break;
                int fds[EXTDISPLAY_DMABUF_MAX_PLANES];
                unsigned nfds = 0;
                if (extdisplay_ipc_recv_fds(ipc, &m, fds, EXTDISPLAY_DMABUF_MAX_PLANES, &nfds) < 0) {
                    if (errno == EAGAIN) continue;
                    break;
                }
                for (unsigned i = 0; i < nfds; i++) {
                    if (fds[i] >= 0) close(fds[i]);
                }
                if (m.op == EXTDISPLAY_IPC_FRAME || m.op == EXTDISPLAY_IPC_DMABUF) {
                    struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK };
                    if (m.op == EXTDISPLAY_IPC_DMABUF && m.length == sizeof(struct extdisplay_dmabuf)) {
                        struct extdisplay_dmabuf di;
                        memcpy(&di, m.text, sizeof di);
                        if (di.generation != 0 && di.slot < EXTDISPLAY_DMABUF_SLOTS &&
                            (int)di.slot != scanning_slot && (int)di.slot != pending_slot) {
                            ack.width = di.slot + 1u;
                        }
                    }
                    extdisplay_ipc_send(ipc, &ack);
                }
                confirmed = m.op == EXTDISPLAY_IPC_CANCELLED;
            }
        }
        if (!confirmed) {
            say("extdisplay: remote input cancel timed out; closing stream");
            kill(*worker, SIGKILL);
            waitpid(*worker, NULL, 0);
            *worker = -1;
            close(ipc);
            ipc = -1;
            active = 0;
            set_state(N_("Disconnected"), N_("Remote input could not be released in time; the stream was closed."));
        }
    }
    outside_cancelled = 1;
    if (base_session_input_cancelled() != 0) return -1;
    if (base_session_input_allowed()) {
        base_raw_enable(0);
        base_raw_enable(2);
        if (active && base_sleep_inhibit(1) == 0) *inhibited = 1;
    }
    return 0;
}

static void read_trimmed_line(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *f = fopen(path, "re");
    if (!f) return;
    if (fgets(out, (int)size, f)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = '\0';
    }
    fclose(f);
}

static void read_identity(char *host, size_t host_sz, char *model, size_t model_sz)
{
    host[0] = model[0] = '\0';
    if (gethostname(host, host_sz) == 0) host[host_sz - 1] = '\0';
    if (!host[0] || !strcmp(host, "(none)") || !strcmp(host, "localhost")) {
        read_trimmed_line("/etc/hostname", host, host_sz);
    }
    read_trimmed_line("/sys/class/dmi/id/product_name", model, model_sz);
    if (!model[0] || !strcmp(model, "System Product Name") || !strcmp(model, "To Be Filled By O.E.M.")) {
        read_trimmed_line("/sys/class/dmi/id/board_name", model, model_sz);
    }
}

static int handle_worker(int *inhibited, double *started)
{
    struct extdisplay_ipc m;
    int fds[EXTDISPLAY_DMABUF_MAX_PLANES] = { -1, -1, -1, -1 };
    unsigned nfds = 0;
    if (extdisplay_ipc_recv_fds(ipc, &m, fds, EXTDISPLAY_DMABUF_MAX_PLANES, &nfds) < 0) {
        if (errno == EAGAIN) return 0;
        say("extdisplay: ipc recv failed (%s)", strerror(errno));
        set_state(N_("Disconnected"), N_("The decoder channel ended."));
        return -1;
    }
    int fd = nfds >= 1 ? fds[0] : -1;
    switch (m.op) {
    case EXTDISPLAY_IPC_READY: {
        if (!(m.width & 3u) || m.width > 31u) break;
        char hello[EXTDISPLAY_IPC_TEXT], host[64], model[64];
        int w, h;
        screen_pixels(&w, &h);
        double scale = screen_scale();
        read_identity(host, sizeof host, model, sizeof model);
        uint32_t vcaps = extdisplay_select_video_caps(m.width, decode_mode);
        say("extdisplay: decoder ready (mode=%s, hw_caps=0x%x -> offer%s%s%s%s)",
            decode_mode, (m.width >> 2) & 7u,
            vcaps & EXTDISPLAY_VCAP_H264 ? " h264" : "",
            vcaps & EXTDISPLAY_VCAP_HEVC420 ? " hevc420" : "",
            vcaps & EXTDISPLAY_VCAP_HEVC444 ? " hevc444" : "",
            vcaps & EXTDISPLAY_VCAP_HEVC ? " hevc" : "");
        int n = extdisplay_hello_full(hello, sizeof hello, (uint32_t)(w > 0 ? w : 1), (uint32_t)(h > 0 ? h : 1),
                          scale >= 0.5 && scale <= 4 ? scale : 1.0, vcaps, host, model,
                          EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET);
        if (send_text(EXTDISPLAY_IPC_HELLO, hello, n, 0) < 0) {
            say("extdisplay: hello send failed (%s)", strerror(errno));
            break;
        }
        return 1;
    }
    case EXTDISPLAY_IPC_ACTIVE:
        active = 1;
        peer_input = (uint32_t)m.width & (EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET);
        pointer_raw_direct(((peer_input & EXTDISPLAY_INPUT_TOUCH) ? POINTER_RAW_TOUCH : 0) |
                           ((peer_input & EXTDISPLAY_INPUT_TABLET) ? POINTER_RAW_TABLET : 0));
        screen_cursor(CURSOR_NONE);
        (void)base_session_hold(1);
        if (base_session_fd() >= 0 && !*inhibited && base_session_input_allowed()) {
            if (base_sleep_inhibit(1) < 0) {
                say("extdisplay: managed sleep inhibitor refused");
                set_state(N_("Unavailable"), N_("The managed sleep inhibitor was not granted."));
                return -1;
            }
            *inhibited = 1;
        }
        *started = uptime();
        set_state(N_("Active"), "");
        return 1;
    case EXTDISPLAY_IPC_FRAME: {
        void *pixels = extdisplay_frame_map(fd, &m);
        close(fd);
        if (!pixels) {
            say("extdisplay: worker sent invalid picture");
            set_state(N_("Disconnected"), N_("The decoder sent an invalid picture."));
            return -1;
        }
        if (frame) munmap(frame, frame_bytes);
        dmabuf_active = 0;
        flip_ack_pending = 0;
        scanning_slot = -1;
        pending_slot = -1;
        frame = pixels;
        frame_bytes = (size_t)m.bytes;
        frame_w = m.width;
        frame_h = m.height;
        struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK };
        if (ipc_send_reliable(&ack, 250) < 0) {
            say("extdisplay: frame ack send failed (%s)", strerror(errno));
            set_state(N_("Disconnected"), N_("The decoder channel ended."));
            return -1;
        }
        return 1;
    }
    case EXTDISPLAY_IPC_DMABUF: {
        struct extdisplay_dmabuf info;
        if (m.length != sizeof info) {
            for (unsigned i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]);
            break;
        }
        memcpy(&info, m.text, sizeof info);
        struct screen_dmabuf_planes planes = {
            .fourcc = info.fourcc,
            .num_planes = info.num_planes,
            .slot = info.slot,
            .generation = info.generation,
            .fds = { -1, -1, -1, -1 },
        };
        for (uint32_t i = 0; i < info.num_planes && i < SCREEN_DMABUF_MAX_PLANES; i++) {
            planes.fds[i] = info.object_index[i] < nfds ? fds[info.object_index[i]] : -1;
            planes.pitches[i] = info.pitches[i];
            planes.offsets[i] = info.offsets[i];
            planes.modifiers[i] = info.modifiers[i];
        }
        /* Reap any completed DRM flip before presenting so screen_present_dmabuf_planes()
         * never silently consumes DRM_EVENT_FLIP_COMPLETE behind our slot tracker's back. */
        if (flip_ack_pending && screen_flip_complete() > 0) {
            int retired = scanning_slot;
            scanning_slot = pending_slot;
            pending_slot = -1;
            flip_ack_pending = 0;
            if (retired >= 0 && retired != scanning_slot && ipc >= 0) {
                struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = (uint32_t)(retired + 1) };
                (void)ipc_send_reliable(&ack, 250);
            }
        }
        errno = 0;
        int ok = screen_present_dmabuf_planes(&planes, m.width, m.height);
        int err = errno;
        for (unsigned i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]);
        int slot_valid = info.generation != 0 && info.slot < EXTDISPLAY_DMABUF_SLOTS;
        if (ok < 0) {
            if (err == ENOENT && slot_valid && info.num_planes == 0) {
                /* Frontend DRM FB cache was cleared (e.g. lease/VT restore): ask
                 * the worker to re-export PRIME fds without disabling dmabuf. */
                scanning_slot = -1;
                pending_slot = -1;
                flip_ack_pending = 0;
                struct extdisplay_ipc ref = { .op = EXTDISPLAY_IPC_REFRESH };
                if (ipc_send_reliable(&ref, 250) < 0) {
                    say("extdisplay: refresh send failed (%s)", strerror(errno));
                    set_state(N_("Disconnected"), N_("The decoder channel ended."));
                    return -1;
                }
                return 0;
            }
            if (err != EAGAIN && err != EBUSY && err != EACCES && err != EPERM) {
                say("extdisplay: dma-buf scanout disabled (%s, fourcc=0x%x, planes=%u, mod=0x%llx); falling back to CPU copy",
                    strerror(err), info.fourcc, info.num_planes, (unsigned long long)info.modifiers[0]);
                dmabuf_active = 0;
                scanning_slot = -1;
                pending_slot = -1;
                struct extdisplay_ipc off = { .op = EXTDISPLAY_IPC_DMABUF_OFF };
                (void)ipc_send_reliable(&off, 250);
            }
            uint32_t released = slot_valid && (int)info.slot != scanning_slot ? info.slot + 1u : 0u;
            struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = released };
            if (ipc_send_reliable(&ack, 250) < 0) {
                say("extdisplay: frame ack send failed (%s)", strerror(errno));
                set_state(N_("Disconnected"), N_("The decoder channel ended."));
                return -1;
            }
            return 0;
        }
        dmabuf_active = 1;
        frame_w = m.width;
        frame_h = m.height;
        viewport();
        if (frame) {
            munmap(frame, frame_bytes);
            frame = NULL;
        }
        if (ok == 1) {
            /* Async DRM page flip in flight: send FRAME_ACK once vblank completes
             * so the worker never overwrites a surface still being scanned out. */
            pending_slot = slot_valid ? (int)info.slot : -1;
            flip_ack_pending = 1;
            flip_sent_at = uptime();
            return 0;
        }
        int retired = scanning_slot;
        scanning_slot = slot_valid ? (int)info.slot : -1;
        pending_slot = -1;
        uint32_t released = retired >= 0 && retired != scanning_slot ? (uint32_t)(retired + 1) : 0u;
        struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = released };
        if (ipc_send_reliable(&ack, 250) < 0) {
            say("extdisplay: frame ack send failed (%s)", strerror(errno));
            set_state(N_("Disconnected"), N_("The decoder channel ended."));
            return -1;
        }
        return 0;
    }
    case EXTDISPLAY_IPC_UNAVAILABLE:
        say("extdisplay: worker unavailable");
        set_state(N_("Unavailable"), N_("The decoder sandbox or FFmpeg is not available."));
        return -1;
    case EXTDISPLAY_IPC_FAILED: {
        char reason[64];
        size_t len = 0;
        uint32_t n = m.length <= EXTDISPLAY_IPC_TEXT ? m.length : EXTDISPLAY_IPC_TEXT;
        for (uint32_t i = 0; i < n && len + 1 < sizeof reason; i++) {
            unsigned char ch = (unsigned char)m.text[i];
            if (ch >= 0x20 && ch <= 0x7e) reason[len++] = (char)ch;
        }
        reason[len] = '\0';
        say("extdisplay: worker ended (%s)", reason[0] ? reason : "unknown");
        set_state(N_("Disconnected"), N_("The sender handshake or the stream failed."));
        return -1;
    }
    case EXTDISPLAY_IPC_CANCELLED:
        return 0;
    default:
        say("extdisplay: unexpected worker op %u", m.op);
        break;
    }
    for (unsigned i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]);
    set_state(N_("Disconnected"), N_("The sender handshake or the stream failed."));
    return -1;
}

static void session_run(int transport)
{
    pid_t worker = -1;
    int owner = base_session_fd(), inhibited = 0, session_started = 0, render_fd = -1;
    char render_path[64] = "";
    snprintf(decode_mode, sizeof decode_mode, "auto");
    extdisplay_card_cfg[0] = '\0';
    display_card_cfg[0] = '\0';
    rc_read(on_rc_config, NULL);
    const char *card_sel = effective_card_selector();
    if (strcmp(decode_mode, "sw") != 0 && screen_output_count() == 1 &&
        screen_render_node_for(card_sel, render_path, sizeof render_path) == 0) {
        render_fd = open(render_path, O_RDWR | O_CLOEXEC);
    }
    set_state(N_("Unavailable"), N_("Waiting for the sender."));
    if (transport < 0) {
        /* nothing to run */
    } else if (owner < 0 && control_fd() < 0) {
        set_state(N_("Unavailable"), N_("No session control channel."));
    } else if (!paired_stream(transport)) {
        set_state(N_("Unavailable"), N_("The transport is not a local stream from the broker."));
    } else if (owner >= 0 && (base_session_service() < 0 || base_session_input_cancelled() != 0)) {
        set_state(N_("Unavailable"), N_("The session owner did not accept this session."));
    } else if ((worker = extdisplay_worker_spawn_hw(transport, render_fd, &ipc)) < 0) {
        set_state(N_("Unavailable"), N_("The decoder worker could not be started."));
    } else {
        set_state(N_("Connecting"), N_("Waiting for the sender."));
        say("extdisplay: decoder worker %d started%s", (int)worker, render_fd >= 0 ? " (VA-API enabled)" : "");
        session_started = 1;
    }
    if (render_fd >= 0) close(render_fd);
    if (transport >= 0) close(transport);
    if (!session_started) {
        if (setup.state == EXTDISPLAY_SETUP_PAIRED) extdisplay_setup_cancel(&setup);
        return;
    }
    screen_cursor(CURSOR_NONE);
    base_raw_enable(2);
    (void)base_session_hold(1);
    draw();

    double started = uptime();
    while (!stop && !leaving) {
        int drm_fd = flip_ack_pending ? screen_flip_fd() : -1;
        if (flip_ack_pending && drm_fd < 0) {
            /* DRM flip completed or was cleared during a lease/flush transition:
             * release worker frame_pending and all busy slots so it never deadlocks. */
            flip_ack_pending = 0;
            scanning_slot = -1;
            pending_slot = -1;
            if (ipc >= 0) {
                struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = EXTDISPLAY_DMABUF_RELEASE_ALL };
                if (ipc_send_reliable(&ack, 250) < 0) {
                    say("extdisplay: frame ack send failed (%s)", strerror(errno));
                    set_state(N_("Disconnected"), N_("The decoder channel ended."));
                    break;
                }
            }
        }
        struct pollfd p[4] = { { ipc, POLLIN, 0 }, { owner, POLLIN, 0 }, { setup.fd, POLLIN, 0 }, { drm_fd, POLLIN, 0 } };
        int key = key_poll_many(p, drm_fd >= 0 ? 4 : 3, 100);
        if (ipc < 0 && (key == K_CLOSE || key == K_ACT_HOME || key == K_ACT_LOCK || key == 27)) break;
        int redraw = 0;
        /* Poll non-blocking flip completion even when key_poll_many returned early
         * on queued input so typing bursts cannot starve DRM flip ACKs. */
        if (flip_ack_pending && drm_fd >= 0 && ((p[3].revents & POLLIN) || key != 0 || uptime() - flip_sent_at >= 0.5)) {
            if (screen_flip_complete() > 0 && ipc >= 0) {
                int retired = scanning_slot;
                scanning_slot = pending_slot;
                pending_slot = -1;
                flip_ack_pending = 0;
                uint32_t released = retired >= 0 && retired != scanning_slot ? (uint32_t)(retired + 1) : 0u;
                struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = released };
                if (ipc_send_reliable(&ack, 250) < 0) {
                    say("extdisplay: frame ack send failed (%s)", strerror(errno));
                    set_state(N_("Disconnected"), N_("The decoder channel ended."));
                    break;
                }
            } else if (flip_ack_pending && uptime() - flip_sent_at >= 0.5 && ipc >= 0) {
                /* Stalled flip event (e.g. lost across modeset/PSR): force a refresh
                 * so video never stays frozen indefinitely. */
                say("extdisplay: DRM flip completion timed out; refreshing dma-buf slots");
                flip_ack_pending = 0;
                scanning_slot = -1;
                pending_slot = -1;
                struct extdisplay_ipc ref = { .op = EXTDISPLAY_IPC_REFRESH };
                if (ipc_send_reliable(&ref, 250) < 0) {
                    say("extdisplay: refresh send failed (%s)", strerror(errno));
                    set_state(N_("Disconnected"), N_("The decoder channel ended."));
                    break;
                }
            }
        }
        if (setup.fd >= 0 && p[2].revents) {
            enum extdisplay_setup_event ev = extdisplay_setup_read(&setup);
            if (ev == EXTDISPLAY_SETUP_EV_ENDED || ev == EXTDISPLAY_SETUP_EV_BROKEN) {
                say("extdisplay: broker ended (%s)", setup.reason[0] ? setup.reason : "broken");
                set_state(N_("Disconnected"), extdisplay_setup_reason_text(setup.reason));
                break;
            }
        }
        if (owner >= 0 && p[1].revents) {
            int policy = base_session_service();
            if (policy < 0) {
                owner = -1;
                if (active) cancel();
                say("extdisplay: session owner ended");
                set_state(N_("Disconnected"), N_("The session owner has gone."));
                break;
            }
            if (policy > 0) {
                key = 0;
                if (release_remote(&worker, &inhibited) < 0) {
                    say("extdisplay: session owner rejected input release");
                    set_state(N_("Disconnected"), N_("The session owner did not accept the release."));
                    break;
                }
                if (session_started && worker < 0) break;
                if (!dmabuf_active) redraw = 1;
                else if (base_session_input_allowed() && !flip_ack_pending && ipc >= 0) {
                    struct extdisplay_ipc ref = {
                        .op = EXTDISPLAY_IPC_REFRESH,
                        .width = scanning_slot >= 0 ? (uint32_t)(scanning_slot + 1) : 0u,
                    };
                    (void)ipc_send_reliable(&ref, 250);
                }
            }
        }
        if (ipc >= 0 && p[0].revents) {
            int r = handle_worker(&inhibited, &started);
            if (r < 0) break;
            redraw |= r > 0;
        }
        if (raw_input() < 0) {
            say("extdisplay: input delivery failed (%s)", strerror(errno));
            set_state(N_("Disconnected"), N_("Remote input could not be delivered."));
            break;
        }
        if (pointer_drain_input(key == K_POINTER) < 0) {
            say("extdisplay: input delivery failed (%s)", strerror(errno));
            set_state(N_("Disconnected"), N_("Remote input could not be delivered."));
            break;
        }
        if (key == K_SHOWN) {
            if (cancel() < 0) {
                say("extdisplay: input delivery failed (%s)", strerror(errno));
                set_state(N_("Disconnected"), N_("Remote input could not be delivered."));
                break;
            }
            if (dmabuf_active && ipc >= 0) {
                scanning_slot = -1;
                pending_slot = -1;
                flip_ack_pending = 0;
                struct extdisplay_ipc ref = { .op = EXTDISPLAY_IPC_REFRESH };
                (void)ipc_send_reliable(&ref, 250);
            }
            redraw = 1;
        }
        if (redraw) draw();
        if (worker > 0 && !active && uptime() - started > HANDSHAKE_S) {
            say("extdisplay: sender handshake timed out");
            set_state(N_("Unavailable"), N_("The sender handshake timed out."));
            break;
        }
    }
    if (active) cancel();
    active = 0;
    pointer_raw_direct(0);
    touch_frame.count = 0;
    peer_input = 0;
    flip_ack_pending = 0;
    scanning_slot = -1;
    pending_slot = -1;
    screen_cursor(CURSOR_ARROW);
    base_raw_enable(1);
    (void)base_session_hold(0);
    if (inhibited) base_sleep_inhibit(0);
    end_worker(worker);
    if (frame) munmap(frame, frame_bytes);
    frame = NULL;
    frame_w = frame_h = 0;
    dmabuf_active = 0;
    if (setup.state == EXTDISPLAY_SETUP_PAIRED) extdisplay_setup_cancel(&setup);
}

static const char *setup_state_text(void)
{
    switch (setup.state) {
    case EXTDISPLAY_SETUP_IDLE: return N_("Idle");
    case EXTDISPLAY_SETUP_SCANNING: return N_("Scanning");
    case EXTDISPLAY_SETUP_LIST: return setup.count ? N_("Ready to connect") : N_("Waiting for a Thunderbolt/USB4 network");
    case EXTDISPLAY_SETUP_PAIRING:
        if (!strcmp(setup.progress, "carrier")) return N_("Waiting for the cable link");
        if (!strcmp(setup.progress, "address")) return N_("Preparing the link-local address");
        if (!strcmp(setup.progress, "discovery")) return N_("Making the receiver discoverable");
        return N_("Preparing the connection");
    case EXTDISPLAY_SETUP_WAITING: return N_("Waiting for the Mac to connect");
    case EXTDISPLAY_SETUP_PAIRED: return N_("Connected");
    case EXTDISPLAY_SETUP_CANCELLING: return N_("Cancelling");
    case EXTDISPLAY_SETUP_RELEASING: return N_("Releasing");
    case EXTDISPLAY_SETUP_RELEASED: return N_("Released");
    case EXTDISPLAY_SETUP_BROKEN: return N_("The pairing helper stopped");
    }
    return N_("Unknown");
}

static void setup_draw(void)
{
    char text[256];
    screen_pixels(&panel_w, &panel_h);
    screen_fill_px(0, 0, panel_w, panel_h, BG);
    screen_bar(_("Extended display"), NULL);
    int rows = screen_rows(), cols = screen_cols();
    if (auto_state.selected >= setup.count) auto_state.selected = setup.count ? setup.count - 1 : 0;
    screen_text(1, 2, _("Connect a Thunderbolt/USB4 link to the Mac."), ACCENT_TEXT, BG);
    if (setup.state == EXTDISPLAY_SETUP_LIST && setup.count > 0 && !diagnostics) {
        for (int i = 0; i < setup.count && i < 8; i++) {
            snprintf(text, sizeof text, "%s %s  %s", i == auto_state.selected ? ">" : " ", setup.ifaces[i].name,
                     setup.ifaces[i].carrier ? _("link ready") : _("waiting for link"));
            screen_text(1, 4 + i, text, i == auto_state.selected ? ACCENT_TEXT : FG, BG);
        }
    }
    if (setup.count == 0) {
        screen_text(1, 4, setup.state == EXTDISPLAY_SETUP_SCANNING ? _("Looking for a Thunderbolt/USB4 network...")
                                                            : _("Waiting for a Thunderbolt/USB4 network."),
                    DIM, BG);
    }
    if (diagnostics && setup.count) {
        for (int i = 0; i < setup.count && i < 8; i++) {
            const struct extdisplay_iface *f = &setup.ifaces[i];
            snprintf(text, sizeof text, "%s  %s  %s  #%d  %s", f->name, f->driver,
                     f->carrier ? _("carrier up") : _("waiting for carrier"), f->ifindex,
                     f->address[0] ? f->address : _("identity unavailable"));
            screen_text(1, 4 + i, text, DIM, BG);
        }
    }
    int top = rows - 8;
    screen_fill(0, top, cols, 3, PANEL);
    if (setup.state == EXTDISPLAY_SETUP_WAITING) {
        screen_text(1, top, _("Waiting for the Mac service to connect automatically..."), ACCENT_TEXT, PANEL);
        if (diagnostics) {
            snprintf(text, sizeof text, _("Interface %s   Address %s   Port %s"), setup.pairing, setup.linklocal, setup.port);
            screen_text(1, top + 1, text, DIM, PANEL);
        }
    } else if (setup.state == EXTDISPLAY_SETUP_PAIRING) {
        screen_text(1, top, _(setup_state_text()), FG, PANEL);
    }
    screen_fill(0, rows - 3, cols, 3, PANEL);
    snprintf(text, sizeof text, _("Status: %s"), _(setup_state_text()));
    screen_text(1, rows - 3, text, ACCENT_TEXT, PANEL);
    if (setup.error[0]) {
        screen_text(1, rows - 2,
                    strcmp(setup.error, "busy") == 0 ? _("The broker is busy: a pairing is still active.") : setup.error,
                    WARN, PANEL);
    } else if (setup.reason[0]) {
        screen_text(1, rows - 2, _(extdisplay_setup_reason_text(setup.reason)), FG, PANEL);
    }
    screen_text(1, rows - 1, _("Enter connect   Esc cancel   F2 details   F1 help   Ctrl+Q quit"), FG, PANEL);
    screen_flush();
}

static const struct ui_help_line setup_keys[] = {
    { "Up, Down", N_("Choose the cable interface") },
    { "Enter", N_("Connect the selected link") },
    { "Esc", N_("Cancel waiting") },
    { "F2", N_("Show interface and address details") },
    { "F5", N_("Rescan cable interfaces") },
    { "F1", N_("Help") },
    { "Super+Esc", N_("Home") },
    { "Ctrl+Q", N_("Quit") },
    { NULL, NULL },
};

static void setup_loop(void)
{
    int quit_ticks = 0;
    extdisplay_auto_init(&auto_state);
    setup_draw();
    while (!stop && setup.state != EXTDISPLAY_SETUP_RELEASED && setup.state != EXTDISPLAY_SETUP_BROKEN) {
        struct pollfd p[1] = { { setup.fd, POLLIN, 0 } };
        int key = key_poll_many(p, 1, 100);
        int redraw = 0;
        if (leaving && ++quit_ticks > 30) break;
        if (raw_input() < 0) break;
        if (p[0].revents) {
            enum extdisplay_setup_event ev = extdisplay_setup_read(&setup);
            if (ev == EXTDISPLAY_SETUP_EV_PAIRED) {
                auto_state.enabled = 0;
                int stream = setup.stream;
                setup.stream = -1;
                session_run(stream);
                /* After a session ends (sender restart or cable unplug), return
                 * to waiting and re-arm automatic pairing for the next link. */
                extdisplay_auto_after_session(&auto_state, &setup);
                redraw = 1;
            } else if (ev != EXTDISPLAY_SETUP_EV_NONE) {
                extdisplay_auto_on_event(&auto_state, &setup, ev);
                redraw = 1;
            }
        }
        int act = key_action(key, keys_modifiers());
        if (act == ACT_HELP) {
            if (ui_help(_("Extended display keys"), setup_keys) == ACT_QUIT) act = ACT_QUIT;
            redraw = 1;
        }
        if (act == ACT_QUIT || key == K_ACT_HOME || key == K_ACT_LOCK) {
            (void)base_session_hold(0);
            leaving = 1;
            if (extdisplay_setup_release(&setup) != 0) break;
            redraw = 1;
        } else if (key == K_SHOWN) {
            redraw = 1;
        } else if (key == K_F1 + 1) {
            diagnostics = !diagnostics;
            redraw = 1;
        } else if (key == K_UP && auto_state.selected > 0) {
            auto_state.enabled = 0;
            auto_state.suppressed = 0;
            auto_state.selected--;
            auto_state.selection = setup.ifaces[auto_state.selected];
            redraw = 1;
        } else if (key == K_DOWN && auto_state.selected + 1 < setup.count) {
            auto_state.enabled = 0;
            auto_state.suppressed = 0;
            auto_state.selected++;
            auto_state.selection = setup.ifaces[auto_state.selected];
            redraw = 1;
        } else if (key == '\n' && setup.state == EXTDISPLAY_SETUP_LIST && setup.count > 0) {
            setup.error[0] = setup.reason[0] = '\0';
            auto_state.enabled = 0;
            auto_state.selection = setup.ifaces[auto_state.selected];
            extdisplay_setup_pair(&setup, setup.ifaces[auto_state.selected].name);
            redraw = 1;
        } else if (key == 27 && (setup.state == EXTDISPLAY_SETUP_PAIRING || setup.state == EXTDISPLAY_SETUP_WAITING)) {
            auto_state.enabled = 0;
            auto_state.suppressed = 1;
            auto_state.suppressed_iface = auto_state.selection;
            extdisplay_setup_cancel(&setup);
            redraw = 1;
        } else if (key == K_F1 + 4 && (setup.state == EXTDISPLAY_SETUP_LIST || setup.state == EXTDISPLAY_SETUP_IDLE)) {
            auto_state.enabled = 0;
            auto_state.suppressed = 1;
            auto_state.suppressed_iface = auto_state.selection;
            extdisplay_setup_scan(&setup);
            redraw = 1;
        }
        if (!leaving && extdisplay_auto_step(&auto_state, &setup)) redraw = 1;
        if (redraw) setup_draw();
    }
    if (setup.state == EXTDISPLAY_SETUP_BROKEN) {
        setup_draw();
        for (;;) {
            int key = key_poll_timeout(-1, 250);
            struct base_raw_key ignored;
            while (base_raw_next(&ignored)) { }
            if (stop || key == 27 || key == '\n' || key == K_CLOSE || key == K_ACT_HOME || key == K_ACT_LOCK) break;
            if (key == K_SHOWN) setup_draw();
        }
    }
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: extdisplay\n");
        return 64;
    }
    signal(SIGTERM, ending);
    signal(SIGINT, ending);
    signal(SIGHUP, ending);
    signal(SIGPIPE, SIG_IGN);
    const char *env_card = getenv("LOCACHY_DRM_CARD");
    int temp_card_env = 0;
    extdisplay_card_cfg[0] = '\0';
    display_card_cfg[0] = '\0';
    rc_read(on_rc_config, NULL);
    if (control_fd() < 0 && (!env_card || !*env_card) &&
        extdisplay_card_cfg[0] && strcmp(extdisplay_card_cfg, "auto") != 0) {
        if (setenv("LOCACHY_DRM_CARD", extdisplay_card_cfg, 1) == 0) {
            temp_card_env = 1;
        }
    }
    int opened = screen_open(BASE_FONT);
    if (temp_card_env) unsetenv("LOCACHY_DRM_CARD");
    if (!opened) return 1;
    keys_open();
    base_raw_enable(1);
    pointer_scroll_events(1);
    pointer_scroll_precision(1);
    base_session_fd();
    int control = -1;
    broker = extdisplay_setup_spawn_broker(EXTDISPLAY_BROKER_PATH, &control);
    if (broker < 0) {
        extdisplay_setup_init(&setup, -1);
        setup.state = EXTDISPLAY_SETUP_BROKEN;
    } else {
        extdisplay_setup_init(&setup, control);
        extdisplay_setup_watch(&setup);
    }
    setup_loop();
    if (setup.fd >= 0) close(setup.fd);
    extdisplay_setup_reap_broker(broker);
    base_raw_enable(0);
    pointer_scroll_events(0);
    pointer_scroll_precision(0);
    return 0;
}
