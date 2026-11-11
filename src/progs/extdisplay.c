/*
 * extdisplay: the extended-display receiver (tools/EXTDISPLAY.md), with no arguments. The
 * peer creates a new virtual output of its own and keeps its original screen;
 * this program shows that new output's pixels and sends LOCachy's keyboard and
 * pointer back. It is not a mirror, a remote login or a VNC viewer, and it
 * creates no compositor: it draws with the base screen and reads the base
 * keyboard and pointer.
 *
 * It starts the broker as its own child over a control socketpair
 * (extdisplay-setup.h: "extdisplay-broker --control-fd 3", nothing else open in the child,
 * the session owner's descriptor included). The setup screen lists the
 * broker's cable interfaces and pairs one; the broker then hands over the
 * paired plaintext AF_UNIX stream in its "paired" message. This program never
 * listens on or dials a network itself. Everything that stream carries is
 * parsed and decoded by extdisplay-worker (extdisplay-ipc.h) in its sandbox; this side only
 * maps sealed, size-checked pixel buffers. Remote input requires the managed
 * session owner (session.h): revocation, owner EOF, focus loss or device loss
 * cancel every remote-held key and button first.
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
#define STATUS_ROWS 4
#define HANDSHAKE_S 8.0

static volatile sig_atomic_t stop;
static int ipc = -1, active, outside_cancelled;
/* The broker, extdisplay's child (extdisplay-setup.h): its channel and the pairing shown. */
static struct extdisplay_setup setup = { .fd = -1, .stream = -1 };
static pid_t broker = -1;
static int leaving, selected;
static uint32_t *frame;
static size_t frame_bytes;
static uint32_t frame_w, frame_h;
static int vx, vy, vw, vh, panel_w, panel_h;
static const char *state = N_("Unavailable");
static const char *detail = N_("A paired dedicated-link transport is required.");

static void ending(int sig) { (void)sig; stop = 1; }

static void set_state(const char *s, const char *d) { state = s; detail = d; }

static int send_text(uint32_t op, const char *text, int n)
{
    if (ipc < 0 || n < 0 || n > (int)EXTDISPLAY_IPC_TEXT) return -1;
    struct extdisplay_ipc m = { .op = op, .length = (uint32_t)n };
    if (n) memcpy(m.text, text, (size_t)n);
    return extdisplay_ipc_send(ipc, &m);
}

/* Remote input only while the owner allows it (the main loop services the
 * policy first each turn); input meanwhile is dropped, cancel never is. */
static int send_input(const char *text, int n)
{
    if (!base_session_input_allowed()) return 0;
    return send_text(EXTDISPLAY_IPC_INPUT, text, n);
}

static int cancel(void)
{
    char text[96];
    outside_cancelled = 1;
    return active ? send_text(EXTDISPLAY_IPC_INPUT, text, extdisplay_cancel_json(text, sizeof text)) : 0;
}

/* The picture's area: below the bar, above the status rows; the stream
 * fitted into it with its aspect ratio. */
static void viewport(void)
{
    int cw, ch, top;
    screen_pixels(&panel_w, &panel_h);
    screen_cells(&cw, &ch, &top);
    int y0 = top + ch, available = panel_h - y0 - ch * STATUS_ROWS;
    if (available < 1) available = 1;
    vx = 0; vy = y0; vw = panel_w > 0 ? panel_w : 1; vh = available;
    if (frame_w && frame_h) {
        if ((uint64_t)vw * frame_h <= (uint64_t)vh * frame_w) vh = (int)((uint64_t)vw * frame_h / frame_w);
        else vw = (int)((uint64_t)vh * frame_w / frame_h);
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
        vx = (panel_w - vw) / 2;
        vy = y0 + (available - vh) / 2;
    }
}

static void draw(void)
{
    viewport();
    screen_fill_px(0, 0, panel_w, panel_h, BG);
    if (frame && frame_w && frame_h) {
        uint32_t *line = malloc((size_t)vw * sizeof *line);
        for (int y = 0; line && y < vh; y++) {
            const uint32_t *src = frame + (size_t)((uint64_t)y * frame_h / (unsigned)vh) * frame_w;
            for (int x = 0; x < vw; x++) line[x] = src[(uint64_t)x * frame_w / (unsigned)vw];
            screen_blit(vx, vy + y, vw, 1, line, vw);
        }
        free(line);
    }
    screen_bar(_("Extended display"), NULL);
    int rows = screen_rows(), cols = screen_cols();
    char text[256];
    screen_fill(0, rows - STATUS_ROWS, cols, STATUS_ROWS, PANEL);
    snprintf(text, sizeof text, _("Status: %s    Stream: %ux%u"), _(state), frame_w, frame_h);
    screen_text(1, rows - 4, text, ACCENT, PANEL);
    screen_text(1, rows - 3, _(detail), FG, PANEL);
    screen_text(1, rows - 2, _("Ctrl+Alt+F12: disconnect    Home, lock and brightness stay local"), FG, PANEL);
    screen_text(1, rows - 1, _("No clipboard, files or commands. The peer keeps its original screen."), DIM, PANEL);
    screen_flush();
}

static int pointer_input(void)
{
    const struct pointer_event *p = pointer_last();
    if (p->x < vx || p->x >= vx + vw || p->y < vy || p->y >= vy + vh) {
        /* Off the picture: release what the peer holds, once. */
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
        /* Content displacement in stream pixels (natural sign: positive dy
         * moves content down, positive dx content right), as LOCachy scrolls
         * it locally. Touch-screen drags are not forwarded: they carry whole
         * steps and no fractional deltas. */
        double kx = frame_w ? (double)frame_w / vw : 1, ky = frame_h ? (double)frame_h / vh : 1;
        n = extdisplay_scroll_json(s, sizeof s, -p->scroll_dx * kx, -p->scroll_dy * ky, p->scroll_phase, p->scroll_source);
    } else if (p->gesture) {
        return 0; /* pinch and swipe are not advertised, never synthesized */
    } else {
        n = extdisplay_pointer_json(s, sizeof s, x, y);
    }
    return send_input(s, n);
}

static int raw_input(void)
{
    struct base_raw_key e;
    while (base_raw_next(&e)) {
        if (e.state == 3) {
            if (cancel() < 0) return -1;
            continue;
        }
        /* evdev KEY_F12: the emergency exit, independent of rc.conf's keys.
         * The broker is released (that ends any session) and extdisplay leaves. */
        if (e.code == 88 && e.state == 1 && (e.modifiers & (KEYS_CTRL | KEYS_ALT)) == (KEYS_CTRL | KEYS_ALT)) {
            leaving = 1;
            if (setup.fd < 0 || extdisplay_setup_release(&setup) != 0) stop = 1;
            return 0;
        }
        if (e.flags & BASE_RAW_LOCAL) {
            /* Process 1 may freeze this program for home or the lock right
             * after this key: release the modifiers already sent first. */
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

/* A bounded end: the worker gets STOP (it sends cancel to the peer), then is
 * killed; a stopped or hostile worker never holds up the local exit. */
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

/* session.h's epoch change: nothing more goes out, the worker writes cancel
 * into the stream and says so (else it is killed: the stream ends, which the
 * peer treats as a release too), and only then the owner gets its ACK. The
 * owner waits 250 ms; this takes at most EXTDISPLAY_CANCEL_WAIT_MS. */
static int release_remote(pid_t *worker, int *inhibited)
{
    if (*inhibited) base_sleep_inhibit(0);
    *inhibited = 0;
    base_raw_enable(0); /* queued keys and held mappings go */
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
                if (fd >= 0) close(fd); /* a picture meanwhile: the next one comes after FRAME_ACK */
                if (m.op == EXTDISPLAY_IPC_FRAME) {
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
    if (active && base_session_input_allowed() && base_sleep_inhibit(1) == 0) *inhibited = 1;
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
        if (!m.width || m.width > 3) break;
        char hello[EXTDISPLAY_IPC_TEXT];
        int cw, ch, top, w, h;
        screen_cells(&cw, &ch, &top);
        screen_pixels(&w, &h);
        h -= top + ch * (1 + STATUS_ROWS);
        double scale = screen_scale();
        int n = extdisplay_hello(hello, sizeof hello, (uint32_t)w, (uint32_t)(h > 0 ? h : 1),
                          scale >= 0.5 && scale <= 4 ? scale : 1.0, m.width & 1, (m.width & 2) != 0);
        if (send_text(EXTDISPLAY_IPC_HELLO, hello, n) < 0) break;
        return 1;
    }
    case EXTDISPLAY_IPC_ACTIVE:
        active = 1;
        /* Held only while remote input is allowed (session.h). */
        if (!*inhibited && base_session_input_allowed()) {
            if (base_sleep_inhibit(1) < 0) {
                set_state(N_("Unavailable"), N_("The managed sleep inhibitor was not granted."));
                return -1;
            }
            *inhibited = 1;
        }
        *started = uptime();
        set_state(N_("Active"), N_("Extended-only companion over the paired dedicated link."));
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
    case EXTDISPLAY_IPC_UNAVAILABLE:
        set_state(N_("Unavailable"), N_("The decoder sandbox or FFmpeg is not available."));
        return -1;
    case EXTDISPLAY_IPC_CANCELLED:
        return 0; /* a late answer to a cancel already given up on */
    default:
        break;
    }
    if (fd >= 0) close(fd);
    set_state(N_("Disconnected"), N_("The companion handshake or the stream failed."));
    return -1;
}

/* One session on the paired stream, until it ends: the peer, the session
 * owner, the broker or the user. The setup screen comes back after it. */
static void session_run(int transport)
{
    pid_t worker = -1;
    int owner = base_session_fd(), inhibited = 0, session_started = 0;
    set_state(N_("Unavailable"), N_("A paired dedicated-link transport is required."));
    if (transport < 0) {
        /* nothing to run */
    } else if (owner < 0) {
        set_state(N_("Unavailable"), N_("No managed session owner: remote input stays disabled."));
    } else if (!paired_stream(transport)) {
        set_state(N_("Unavailable"), N_("The transport is not a paired local stream from the broker."));
    } else if (base_session_service() < 0 || base_session_input_cancelled() != 0) {
        /* Nothing is held yet: the first policy is acknowledged at once. */
        set_state(N_("Unavailable"), N_("The session owner did not accept this session."));
    } else if ((worker = extdisplay_worker_spawn(transport, &ipc)) < 0) {
        set_state(N_("Unavailable"), N_("The decoder worker could not be started safely."));
    } else {
        set_state(N_("Connecting"), N_("Waiting for the companion's capabilities."));
        say("extdisplay: decoder worker %d started", (int)worker);
        session_started = 1;
    }
    if (transport >= 0) close(transport);
    draw();

    double started = uptime();
    while (!stop && !leaving) {
        struct pollfd p[3] = { { ipc, POLLIN, 0 }, { owner, POLLIN, 0 }, { setup.fd, POLLIN, 0 } };
        int key = key_poll_many(p, 3, 100);
        if (key == K_CLOSE || key == K_ACT_HOME || key == K_ACT_LOCK || (ipc < 0 && key == 27)) break;
        int redraw = 0;
        /* The broker ends the session (the cable, a timeout), or has gone. */
        if (setup.fd >= 0 && p[2].revents) {
            enum extdisplay_setup_event ev = extdisplay_setup_read(&setup);
            if (ev == EXTDISPLAY_SETUP_EV_ENDED || ev == EXTDISPLAY_SETUP_EV_BROKEN) {
                set_state(N_("Disconnected"), extdisplay_setup_reason_text(setup.reason));
                break;
            }
        }
        /* Policy first, every turn, also without a session: a hang-up here
         * must not spin the loop. Nothing gathered before a change goes out
         * after its cancel. */
        if (owner >= 0 && p[1].revents) {
            int policy = base_session_service();
            if (policy < 0) {
                owner = -1;
                if (active) cancel();
                set_state(N_("Disconnected"), N_("The session owner has gone."));
                break;
            }
            if (policy > 0) {
                key = 0; /* a pointer read before the change is stale */
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
            cancel(); /* the screen was away: what the peer held is stale */
            redraw = 1;
        }
        if (redraw) draw();
        if (worker > 0 && !active && uptime() - started > HANDSHAKE_S) {
            set_state(N_("Unavailable"), N_("The companion handshake timed out."));
            break;
        }
    }
    /* session.h's order: no input, cancel, inhibitor off, then the worker
     * (STOP writes cancel again before it exits). */
    if (active) cancel();
    active = 0;
    if (inhibited) base_sleep_inhibit(0);
    end_worker(worker);
    if (frame) munmap(frame, frame_bytes);
    frame = NULL;
    frame_w = frame_h = 0;
    /* Ended here first (the peer, the owner, the user): the broker is told,
     * and answers "ended cancelled" once it is idle again. */
    if (setup.state == EXTDISPLAY_SETUP_PAIRED) extdisplay_setup_cancel(&setup);
}

/* The setup screen: the candidate interfaces with their carrier, and the
 * pairing region (address, port, code and the Mac's command shape, drawn only
 * while the broker waits for the sender), the status line and the keys. */
static const char *setup_state_text(void)
{
    switch (setup.state) {
    case EXTDISPLAY_SETUP_IDLE: return N_("Idle");
    case EXTDISPLAY_SETUP_SCANNING: return N_("Scanning");
    case EXTDISPLAY_SETUP_LIST: return N_("Choose an interface");
    case EXTDISPLAY_SETUP_PAIRING: return N_("Asking the broker for a code");
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
    screen_text(1, 2, _("Choose the cable interface to pair with the Mac."), ACCENT, BG);
    if (setup.count == 0) {
        screen_text(1, 4, setup.state == EXTDISPLAY_SETUP_SCANNING ? _("Scanning...")
                                                            : _("No cable interface found. Connect the cable, then press F5."),
                    DIM, BG);
    }
    for (int i = 0; i < setup.count && i < 8; i++) {
        const struct extdisplay_iface *f = &setup.ifaces[i];
        snprintf(text, sizeof text, "%s %-16s %-20s %s", i == selected ? ">" : " ", f->name, f->driver,
                 f->carrier ? _("carrier up") : _("no carrier"));
        screen_text(1, 4 + i, text, i == selected ? ACCENT : FG, BG);
    }
    int top = rows - 9;
    screen_fill(0, top, cols, 4, PANEL);
    if (setup.state == EXTDISPLAY_SETUP_WAITING) {
        snprintf(text, sizeof text, _("Address  %s     Port  %s"), setup.linklocal, setup.port);
        screen_text(1, top, text, ACCENT, PANEL);
        snprintf(text, sizeof text, _("Code     %s"), setup.code);
        screen_text(1, top + 1, text, ACCENT, PANEL);
        snprintf(text, sizeof text,
                 _("On the Mac: locachy-macos-sender --interface <mac-interface> --address %s --port %s"),
                 setup.linklocal, setup.port);
        screen_text(1, top + 2, text, FG, PANEL);
    } else if (setup.state == EXTDISPLAY_SETUP_PAIRING) {
        screen_text(1, top, _("Asking the broker for an address and a code..."), FG, PANEL);
    }
    screen_fill(0, rows - 4, cols, 4, PANEL);
    snprintf(text, sizeof text, _("Status: %s"), _(setup_state_text()));
    screen_text(1, rows - 4, text, ACCENT, PANEL);
    if (setup.error[0]) {
        screen_text(1, rows - 3,
                    strcmp(setup.error, "busy") == 0 ? _("The broker is busy: a pairing is still active.") : setup.error,
                    WARN, PANEL);
    } else if (setup.reason[0]) {
        screen_text(1, rows - 3, _(extdisplay_setup_reason_text(setup.reason)), FG, PANEL);
    }
    screen_text(1, rows - 2, _("Up/Down choose   Enter pair   Esc cancel   F5 scan   Ctrl+Alt+F12 release"), FG, PANEL);
    screen_text(1, rows - 1, _("The code and the address are kept in memory only."), DIM, PANEL);
    screen_flush();
}

/* Pairs and sessions one after another until the broker releases (or stops). */
static void setup_loop(void)
{
    int quit_ticks = 0;
    setup_draw();
    while (!stop && setup.state != EXTDISPLAY_SETUP_RELEASED && setup.state != EXTDISPLAY_SETUP_BROKEN) {
        struct pollfd p[1] = { { setup.fd, POLLIN, 0 } };
        int key = key_poll_many(p, 1, 100);
        int redraw = 0;
        /* Leaving but no answer from the broker in 3 s: leave anyway. */
        if (leaving && ++quit_ticks > 30) break;
        if (raw_input() < 0) break;
        if (p[0].revents) {
            switch (extdisplay_setup_read(&setup)) {
            case EXTDISPLAY_SETUP_EV_NONE:
                break;
            case EXTDISPLAY_SETUP_EV_PAIRED: {
                int stream = setup.stream;
                setup.stream = -1;
                session_run(stream);
                if (setup.state == EXTDISPLAY_SETUP_IDLE || setup.state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(&setup);
                redraw = 1;
                break;
            }
            case EXTDISPLAY_SETUP_EV_ENDED:
                /* The device is back on the host: its carrier may have changed. */
                if (setup.state == EXTDISPLAY_SETUP_IDLE || setup.state == EXTDISPLAY_SETUP_LIST) extdisplay_setup_scan(&setup);
                redraw = 1;
                break;
            default:
                redraw = 1;
                break;
            }
            if (setup.state == EXTDISPLAY_SETUP_LIST && setup.count && selected >= setup.count) selected = 0;
        }
        if (key == K_CLOSE || key == K_ACT_HOME || key == K_ACT_LOCK) {
            /* Leaving: the broker releases and answers "ended released"; the
             * loop ends then. Without a channel there is nothing to wait for. */
            leaving = 1;
            if (extdisplay_setup_release(&setup) != 0) break;
            redraw = 1;
        } else if (key == K_SHOWN) {
            redraw = 1;
        } else if (key == K_UP && selected > 0) {
            selected--;
            redraw = 1;
        } else if (key == K_DOWN && selected + 1 < setup.count) {
            selected++;
            redraw = 1;
        } else if (key == '\n' && setup.state == EXTDISPLAY_SETUP_LIST && setup.count > 0) {
            setup.error[0] = setup.reason[0] = '\0';
            extdisplay_setup_pair(&setup, setup.ifaces[selected].name);
            redraw = 1;
        } else if (key == 27 && (setup.state == EXTDISPLAY_SETUP_PAIRING || setup.state == EXTDISPLAY_SETUP_WAITING)) {
            extdisplay_setup_cancel(&setup);
            redraw = 1;
        } else if (key == K_F1 + 4 && (setup.state == EXTDISPLAY_SETUP_LIST || setup.state == EXTDISPLAY_SETUP_IDLE)) {
            setup.error[0] = setup.reason[0] = '\0';
            extdisplay_setup_scan(&setup);
            redraw = 1;
        }
        if (redraw) setup_draw();
    }
    if (setup.state == EXTDISPLAY_SETUP_BROKEN) {
        /* Shown until dismissed: the reason is on screen, the helper is gone. */
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
    /* Takes the session owner's descriptor out of reach of everything below
     * (the broker is started with every other descriptor closed). */
    base_session_fd();
    int control = -1;
    broker = extdisplay_setup_spawn_broker(EXTDISPLAY_BROKER_PATH, &control);
    if (broker < 0) {
        extdisplay_setup_init(&setup, -1);
        setup.state = EXTDISPLAY_SETUP_BROKEN;
    } else {
        extdisplay_setup_init(&setup, control);
        extdisplay_setup_scan(&setup);
    }
    setup_loop();
    if (setup.fd >= 0) close(setup.fd); /* the broker sees the end: it releases and exits */
    extdisplay_setup_reap_broker(broker);
    base_raw_enable(0);
    pointer_scroll_events(0);
    pointer_scroll_precision(0);
    return 0;
}

