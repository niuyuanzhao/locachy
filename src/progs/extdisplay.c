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

#define BG     0x101820
#define PANEL  0x203048
#define FG     0xD0D0D0
#define ACCENT 0xD0E8FF
#define DIM    0x90A8C0
#define WARN   0xFFB080
#define HANDSHAKE_S 8.0

static volatile sig_atomic_t stop;
static int ipc = -1, active, outside_cancelled;
static struct extdisplay_setup setup = { .fd = -1, .stream = -1 };
static pid_t broker = -1;
static int leaving, selected, diagnostics, auto_pair = 1;
static struct extdisplay_iface selection, suppressed;
static int suppress_auto;

static int same_interface(const struct extdisplay_iface *a, const struct extdisplay_iface *b)
{
    return a->ifindex == b->ifindex && !strcmp(a->address, b->address);
}
static int unplugged;
static uint32_t *frame;
static size_t frame_bytes;
static uint32_t frame_w, frame_h;
static int vx, vy, vw, vh, panel_w, panel_h;
static char decode_mode[16] = "auto";
static const char *state = N_("Unavailable");
static const char *detail = N_("Waiting for the sender.");

static void on_rc_decode(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (strcmp(name, "extdisplay_decode") == 0 &&
        (strcmp(value, "auto") == 0 || strcmp(value, "hw") == 0 || strcmp(value, "sw") == 0)) {
        snprintf(decode_mode, sizeof decode_mode, "%s", value);
    }
}

static void ending(int sig) { (void)sig; stop = 1; }

static void set_state(const char *s, const char *d) { state = s; detail = d; }

static int send_text(uint32_t op, const char *text, int n)
{
    if (ipc < 0 || n < 0 || n > (int)EXTDISPLAY_IPC_TEXT) return -1;
    struct extdisplay_ipc m = { .op = op, .length = (uint32_t)n };
    if (n) memcpy(m.text, text, (size_t)n);
    return extdisplay_ipc_send(ipc, &m);
}

static int input_allowed(void)
{
    return base_session_fd() >= 0 ? base_session_input_allowed() : control_fd() >= 0;
}

static int send_input(const char *text, int n)
{
    if (!input_allowed()) return 0;
    return send_text(EXTDISPLAY_IPC_INPUT, text, n);
}

static int cancel(void)
{
    char text[96];
    outside_cancelled = 1;
    return active ? send_text(EXTDISPLAY_IPC_INPUT, text, extdisplay_cancel_json(text, sizeof text)) : 0;
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
    } else {
        screen_fill_px(0, 0, panel_w, panel_h, 0x000000);
    }
    screen_flush();
}

static int pointer_input(void)
{
    const struct pointer_event *p = pointer_last();
    if (p->x < vx || p->x >= vx + vw || p->y < vy || p->y >= vy + vh) {
        return outside_cancelled ? 0 : cancel();
    }
    outside_cancelled = 0;
    double x = vw > 1 ? (double)(p->x - vx) / (vw - 1) : 0;
    double y = vh > 1 ? (double)(p->y - vy) / (vh - 1) : 0;
    char s[256];
    int n;
    if (p->button) {
        n = extdisplay_button_json(s, sizeof s, (uint32_t)p->button, p->pressed, x, y);
    } else if (p->scroll_dx || p->scroll_dy || (p->gesture == GESTURE_SCROLL && p->scroll_source != SCROLL_TOUCH)) {
        double kx = frame_w ? (double)frame_w / vw : 1, ky = frame_h ? (double)frame_h / vh : 1;
        n = extdisplay_scroll_json(s, sizeof s, -p->scroll_dx * kx, -p->scroll_dy * ky, p->scroll_phase, p->scroll_source);
    } else if (p->gesture) {
        return 0;
    } else if (p->dx || p->dy) {
        n = extdisplay_pointer_delta_json(s, sizeof s, x, y, p->dx, p->dy);
    } else {
        n = extdisplay_pointer_json(s, sizeof s, x, y);
    }
    return send_input(s, n);
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
        if (n >= 0 && send_input(text, n) < 0) return -1;
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
    if (*worker > 0 && ipc >= 0) {
        struct extdisplay_ipc m = { .op = EXTDISPLAY_IPC_CANCEL };
        int confirmed = 0;
        double deadline = uptime() + EXTDISPLAY_CANCEL_WAIT_MS / 1000.0;
        if (extdisplay_ipc_send(ipc, &m) == 0) {
            while (!confirmed) {
                int left = (int)((deadline - uptime()) * 1000);
                struct pollfd p = { ipc, POLLIN, 0 };
                if (left <= 0 || poll(&p, 1, left) <= 0) break;
                int fd;
                if (extdisplay_ipc_recv(ipc, &m, &fd) < 0) {
                    if (errno == EAGAIN) continue;
                    break;
                }
                if (fd >= 0) close(fd);
                if (m.op == EXTDISPLAY_IPC_FRAME || m.op == EXTDISPLAY_IPC_DMABUF) {
                    struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK };
                    extdisplay_ipc_send(ipc, &ack);
                }
                confirmed = m.op == EXTDISPLAY_IPC_CANCELLED;
            }
        }
        if (!confirmed) {
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

static int handle_worker(int *inhibited, double *started)
{
    struct extdisplay_ipc m;
    int fd;
    if (extdisplay_ipc_recv(ipc, &m, &fd) < 0) {
        if (errno == EAGAIN) return 0;
        set_state(N_("Disconnected"), N_("The decoder channel ended."));
        return -1;
    }
    switch (m.op) {
    case EXTDISPLAY_IPC_READY: {
        if (!(m.width & 3u) || m.width > 31u) break;
        char hello[EXTDISPLAY_IPC_TEXT];
        int w, h;
        screen_pixels(&w, &h);
        double scale = screen_scale();
        int use_hevc = (m.width & 2u) != 0;
        if (strcmp(decode_mode, "hw") == 0 && !(m.width & 16u) && (m.width & 4u)) use_hevc = 0;
        int n = extdisplay_hello(hello, sizeof hello, (uint32_t)(w > 0 ? w : 1), (uint32_t)(h > 0 ? h : 1),
                          scale >= 0.5 && scale <= 4 ? scale : 1.0, m.width & 1u, use_hevc);
        if (send_text(EXTDISPLAY_IPC_HELLO, hello, n) < 0) break;
        return 1;
    }
    case EXTDISPLAY_IPC_ACTIVE:
        active = 1;
        screen_cursor(CURSOR_NONE);
        (void)base_session_hold(1);
        if (base_session_fd() >= 0 && !*inhibited && base_session_input_allowed()) {
            if (base_sleep_inhibit(1) < 0) {
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
            set_state(N_("Disconnected"), N_("The decoder sent an invalid picture."));
            return -1;
        }
        if (frame) munmap(frame, frame_bytes);
        frame = pixels;
        frame_bytes = (size_t)m.bytes;
        frame_w = m.width;
        frame_h = m.height;
        struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK };
        return extdisplay_ipc_send(ipc, &ack) < 0 ? -1 : 1;
    }
    case EXTDISPLAY_IPC_DMABUF: {
        struct extdisplay_dmabuf info;
        if (m.length != sizeof info) {
            if (fd >= 0) close(fd);
            break;
        }
        memcpy(&info, m.text, sizeof info);
        int ok = screen_present_dmabuf(fd, m.width, m.height, info.fourcc, info.pitch, info.offset, info.modifier);
        close(fd);
        if (ok < 0) {
            struct extdisplay_ipc off = { .op = EXTDISPLAY_IPC_DMABUF_OFF };
            (void)extdisplay_ipc_send(ipc, &off);
        } else if (frame) {
            munmap(frame, frame_bytes);
            frame = NULL;
            frame_w = frame_h = 0;
        }
        struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK };
        return extdisplay_ipc_send(ipc, &ack) < 0 ? -1 : 0;
    }
    case EXTDISPLAY_IPC_UNAVAILABLE:
        set_state(N_("Unavailable"), N_("The decoder sandbox or FFmpeg is not available."));
        return -1;
    case EXTDISPLAY_IPC_CANCELLED:
        return 0;
    default:
        break;
    }
    if (fd >= 0) close(fd);
    set_state(N_("Disconnected"), N_("The sender handshake or the stream failed."));
    return -1;
}

static void session_run(int transport)
{
    pid_t worker = -1;
    int owner = base_session_fd(), inhibited = 0, session_started = 0, render_fd = -1;
    char render_path[64];
    snprintf(decode_mode, sizeof decode_mode, "auto");
    rc_read(on_rc_decode, NULL);
    if (strcmp(decode_mode, "sw") != 0 && screen_output_count() == 1 &&
        screen_render_node(render_path, sizeof render_path) == 0) {
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
        struct pollfd p[3] = { { ipc, POLLIN, 0 }, { owner, POLLIN, 0 }, { setup.fd, POLLIN, 0 } };
        int key = key_poll_many(p, 3, 100);
        if (ipc < 0 && (key == K_CLOSE || key == K_ACT_HOME || key == K_ACT_LOCK || key == 27)) break;
        int redraw = 0;
        if (setup.fd >= 0 && p[2].revents) {
            enum extdisplay_setup_event ev = extdisplay_setup_read(&setup);
            if (ev == EXTDISPLAY_SETUP_EV_ENDED || ev == EXTDISPLAY_SETUP_EV_BROKEN) {
                if (ev == EXTDISPLAY_SETUP_EV_ENDED && strcmp(setup.reason, "carrier-lost") == 0) unplugged = 1;
                set_state(N_("Disconnected"), extdisplay_setup_reason_text(setup.reason));
                break;
            }
        }
        if (owner >= 0 && p[1].revents) {
            int policy = base_session_service();
            if (policy < 0) {
                owner = -1;
                if (active) cancel();
                set_state(N_("Disconnected"), N_("The session owner has gone."));
                break;
            }
            if (policy > 0) {
                key = 0;
                if (release_remote(&worker, &inhibited) < 0) {
                    set_state(N_("Disconnected"), N_("The session owner did not accept the release."));
                    break;
                }
                if (session_started && worker < 0) break;
                redraw = 1;
            }
        }
        if (ipc >= 0 && p[0].revents) {
            int r = handle_worker(&inhibited, &started);
            if (r < 0) break;
            redraw |= r > 0;
        }
        if (raw_input() < 0) {
            set_state(N_("Disconnected"), N_("Remote input could not be delivered."));
            break;
        }
        if (active && key == K_POINTER && pointer_input() < 0) {
            set_state(N_("Disconnected"), N_("Remote input could not be delivered."));
            break;
        }
        if (key == K_SHOWN) {
            cancel();
            redraw = 1;
        }
        if (redraw) draw();
        if (worker > 0 && !active && uptime() - started > HANDSHAKE_S) {
            set_state(N_("Unavailable"), N_("The sender handshake timed out."));
            break;
        }
    }
    if (active) cancel();
    active = 0;
    screen_cursor(CURSOR_ARROW);
    base_raw_enable(1);
    (void)base_session_hold(0);
    if (inhibited) base_sleep_inhibit(0);
    end_worker(worker);
    if (frame) munmap(frame, frame_bytes);
    frame = NULL;
    frame_w = frame_h = 0;
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
    if (selected >= setup.count) selected = setup.count ? setup.count - 1 : 0;
    screen_text(1, 2, _("Connect a Thunderbolt/USB4 link to the Mac."), ACCENT, BG);
    if (setup.state == EXTDISPLAY_SETUP_LIST && setup.count > 0 && !diagnostics) {
        for (int i = 0; i < setup.count && i < 8; i++) {
            snprintf(text, sizeof text, "%s %s  %s", i == selected ? ">" : " ", setup.ifaces[i].name,
                     setup.ifaces[i].carrier ? _("link ready") : _("waiting for link"));
            screen_text(1, 4 + i, text, i == selected ? ACCENT : FG, BG);
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
        screen_text(1, top, _("Waiting for the Mac service to connect automatically..."), ACCENT, PANEL);
        if (diagnostics) {
            snprintf(text, sizeof text, _("Interface %s   Address %s   Port %s"), setup.pairing, setup.linklocal, setup.port);
            screen_text(1, top + 1, text, DIM, PANEL);
        }
    } else if (setup.state == EXTDISPLAY_SETUP_PAIRING) {
        screen_text(1, top, _(setup_state_text()), FG, PANEL);
    }
    screen_fill(0, rows - 3, cols, 3, PANEL);
    snprintf(text, sizeof text, _("Status: %s"), _(setup_state_text()));
    screen_text(1, rows - 3, text, ACCENT, PANEL);
    if (setup.error[0]) {
        screen_text(1, rows - 2,
                    strcmp(setup.error, "busy") == 0 ? _("The broker is busy: a pairing is still active.") : setup.error,
                    WARN, PANEL);
    } else if (setup.reason[0]) {
        screen_text(1, rows - 2, _(extdisplay_setup_reason_text(setup.reason)), FG, PANEL);
    }
    if (unplugged) {
        screen_fill(0, rows - 2, cols, 1, PANEL);
        screen_text(1, rows - 2, _("The cable was unplugged. Press any key to close."), WARN, PANEL);
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
    setup_draw();
    while (!stop && setup.state != EXTDISPLAY_SETUP_RELEASED && setup.state != EXTDISPLAY_SETUP_BROKEN) {
        struct pollfd p[1] = { { setup.fd, POLLIN, 0 } };
        int key = key_poll_many(p, 1, 100);
        int redraw = 0;
        if (leaving && ++quit_ticks > 30) break;
        if (raw_input() < 0) break;
        if (p[0].revents) {
            switch (extdisplay_setup_read(&setup)) {
            case EXTDISPLAY_SETUP_EV_NONE:
                break;
            case EXTDISPLAY_SETUP_EV_CHANGED:
                if (!unplugged && (setup.state == EXTDISPLAY_SETUP_LIST || setup.state == EXTDISPLAY_SETUP_SCANNING)) auto_pair = 1;
                redraw = 1;
                break;
            case EXTDISPLAY_SETUP_EV_LIST: {
                int blocked_present = 0;
                for (int i = 0; i < setup.count; i++) {
                    if (same_interface(&selection, &setup.ifaces[i])) selected = i;
                    if (same_interface(&suppressed, &setup.ifaces[i])) {
                        blocked_present = 1;
                        if (setup.ifaces[i].carrier != suppressed.carrier) {
                            suppressed.carrier = setup.ifaces[i].carrier;
                            if (suppressed.carrier) suppress_auto = 0;
                        }
                    }
                }
                if (!blocked_present) suppress_auto = 0;
                redraw = 1;
                break;
            }
            case EXTDISPLAY_SETUP_EV_ERROR:
                auto_pair = 0;
                suppress_auto = 1;
                suppressed = selection;
                if (setup.state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(&setup);
                redraw = 1;
                break;
            case EXTDISPLAY_SETUP_EV_CODE:
                redraw = 1;
                break;
            case EXTDISPLAY_SETUP_EV_PAIRED: {
                auto_pair = 0;
                int stream = setup.stream;
                setup.stream = -1;
                session_run(stream);
                /* After a session ends without unplugging, immediately re-arm
                 * so a reconnected sender can attach again without user action. */
                auto_pair = !unplugged;
                suppress_auto = unplugged;
                suppressed = selection;
                if (setup.state == EXTDISPLAY_SETUP_IDLE || setup.state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(&setup);
                redraw = 1;
                break;
            }
            case EXTDISPLAY_SETUP_EV_ENDED:
                if (strcmp(setup.reason, "cancelled") != 0) {
                    suppress_auto = unplugged;
                }
                auto_pair = !suppress_auto;
                suppressed = selection;
                if (setup.state == EXTDISPLAY_SETUP_IDLE || setup.state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(&setup);
                redraw = 1;
                break;
            default:
                redraw = 1;
                break;
            }
            if (setup.state == EXTDISPLAY_SETUP_LIST && setup.count && selected >= setup.count) selected = 0;
        }
        if (unplugged && key && key != K_SHOWN && key != K_POINTER) {
            key = K_CLOSE;
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
        } else if (key == K_UP && selected > 0) {
            auto_pair = 0;
            suppress_auto = 0;
            selected--;
            selection = setup.ifaces[selected];
            redraw = 1;
        } else if (key == K_DOWN && selected + 1 < setup.count) {
            auto_pair = 0;
            suppress_auto = 0;
            selected++;
            selection = setup.ifaces[selected];
            redraw = 1;
        } else if (key == '\n' && setup.state == EXTDISPLAY_SETUP_LIST && setup.count > 0) {
            setup.error[0] = setup.reason[0] = '\0';
            auto_pair = 0;
            selection = setup.ifaces[selected];
            extdisplay_setup_pair(&setup, setup.ifaces[selected].name);
            redraw = 1;
        } else if (key == 27 && (setup.state == EXTDISPLAY_SETUP_PAIRING || setup.state == EXTDISPLAY_SETUP_WAITING)) {
            auto_pair = 0;
            suppress_auto = 1;
            suppressed = selection;
            extdisplay_setup_cancel(&setup);
            redraw = 1;
        } else if (key == K_F1 + 4 && (setup.state == EXTDISPLAY_SETUP_LIST || setup.state == EXTDISPLAY_SETUP_IDLE)) {
            auto_pair = 0;
            suppress_auto = 1;
            suppressed = selection;
            extdisplay_setup_scan(&setup);
            redraw = 1;
        }
        if (!leaving && !unplugged && setup.state == EXTDISPLAY_SETUP_LIST) {
            if (setup.changed) {
                extdisplay_setup_scan(&setup);
                redraw = 1;
            } else if (auto_pair && !suppress_auto) {
                int choice = extdisplay_setup_automatic(&setup);
                if (choice >= 0) {
                    selected = choice;
                    selection = setup.ifaces[selected];
                    setup.error[0] = setup.reason[0] = '\0';
                    auto_pair = 0;
                    extdisplay_setup_pair(&setup, selection.name);
                    redraw = 1;
                }
            }
        }
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
    if (!screen_open(BASE_FONT)) return 1;
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
