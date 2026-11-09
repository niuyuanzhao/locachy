/*
 * Rescue is an independent native full-screen program, not a shell or term
 * mode. locachy-rescue owns the storage/link policy; this UI sends only its
 * fixed argv and a secret through stdin, and displays its structured reply.
 * A single UI holds ui.lock for the whole session. Nothing is unlocked or
 * enabled at startup. Only a successful, verified cleanup permits exit.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "clipboard.h"
#include "internal.h"
#include "textsel.h"

#ifndef RESCUE_HELPER
#define RESCUE_HELPER "/usr/local/bin/locachy-rescue"
#endif
#ifndef RESCUE_TERM
#define RESCUE_TERM "/usr/local/bin/term"
#endif
#ifndef RESCUE_RUN_DIR
#define RESCUE_RUN_DIR "/run/locachy-rescue"
#endif
#ifndef RESCUE_SYSTEMCTL
#define RESCUE_SYSTEMCTL "/usr/bin/systemctl"
#endif
#ifndef RESCUE_KEYTABLE
#define RESCUE_KEYTABLE "/usr/local/lib/locachy/keytable"
#endif
#ifndef RESCUE_KEYMAP
#define RESCUE_KEYMAP RESCUE_RUN_DIR "/keymap.table"
#endif
#ifndef RESCUE_KEYMAP_TMP
#define RESCUE_KEYMAP_TMP RESCUE_RUN_DIR "/keymap.table.tmp"
#endif
#ifndef RESCUE_TIMEOUT_MS
#define RESCUE_TIMEOUT_MS 300000
#endif
#ifndef RESCUE_KILL_MS
#define RESCUE_KILL_MS 2000
#endif
#define REPLY_MAX (1024 * 1024)
#define ITEMS_MAX 128
#define SECRET_MAX 4096
#define LOG_MAX 512
#define LINE_MAXIMUM 1024
#define FG 0xD0D0D0
#define BG 0x101820
#define ACCENT 0x80C0FF
#define DIM 0x808890
#define ERROR 0xFF9090
#define PANE 0x05080C
#define BAR 0x304860
#define HIT_ACTION 100
#define HIT_KEY 1000
#define HIT_STATUS 2000
#define HIT_OUTPUT 2001
#define HIT_PICK 3000

enum { A_SCAN, A_STATUS, A_UNLOCK, A_REPAIR, A_CLOSE, A_START_LINK, A_STOP_LINK, A_TERMINAL, A_KEYMAP, A_EXIT, NACTIONS };
static const char *const actions[NACTIONS] = {
    N_("Rescan disks and interfaces"), N_("Inspect status (read-only)"), N_("Unlock target read-only"),
    N_("Enable repair (read/write)"), N_("Close target"), N_("Start debug link"), N_("Stop debug link"),
    N_("Target terminal"), N_("Keyboard layout"), N_("Exit Rescue"),
};
enum { F_ACTIONS, F_STATUS, F_OUTPUT };
enum { M_NONE, M_DEVICE, M_INTERFACE, M_UNLOCK, M_REPAIR, M_LINK, M_LINK_AGAIN, M_CONFIRM, M_LAYOUT };
struct device { char path[256], label[256], uuid[128], model[256]; long long size; };
struct interface { char name[128], address[128]; int carrier; };
struct snapshot {
    char state[24], device[256], uuid[128], mount[512], swap[256], esp[256], esp_reason[512];
    int repair_blocked;
    struct device devices[ITEMS_MAX];
    struct interface interfaces[ITEMS_MAX];
    int ndevices, ninterfaces, active, port;
    char interface[128], address[256], fingerprint[256], link_state[128];
};
static struct snapshot state;
static int known, selected, action_top, focus, status_scroll, output_scroll, modal, pick_sel, pick_top, pick_offset;
static char pending[256], filter[128], secret[SECRET_MAX + 1], first_secret[SECRET_MAX + 1];
static size_t secret_pos;
static int reveal, exiting, done, notice_error, own_keyboard;
static int boot_session, session_closed, closed_selected, power_confirmation, signal_exit;
static char notice[1024], ssh[768], keyboard[128], next_keyboard[128];
static char log_lines[LOG_MAX][LINE_MAXIMUM], partial[LINE_MAXIMUM];
static int log_count;
static char status_lines[128][LINE_MAXIMUM];
static int nstatus, status_h, list_y, list_h, output_x, output_y, output_h, action_w;
static int console_fd = -1, console_kb = K_UNICODE, lock_fd = -1, handed_screen;
static volatile sig_atomic_t interrupted;
struct child {
    pid_t pid;
    int in, out, err, reaped, status, stopping, failed, terminal, power;
    double deadline, stop_at, drain_at;
    char command[32], input[SECRET_MAX + 1];
    size_t input_len, sent, len;
    char *reply;
};
static struct child child = { .in = -1, .out = -1, .err = -1 };

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1000000;
}
static void tell(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tell(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notice, sizeof notice, fmt, ap);
    va_end(ap);
    notice_error = error;
}
static void wipe_secrets(void)
{
    explicit_bzero(secret, sizeof secret);
    explicit_bzero(first_secret, sizeof first_secret);
    secret_pos = 0;
    reveal = 0;
}
static void close_fd(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}
static void console_restore(void)
{
    if (console_fd < 0) return;
    ioctl(console_fd, KDSKBMODE, console_kb);
    ioctl(console_fd, KDSETMODE, KD_TEXT);
    tcflush(console_fd, TCIFLUSH);
}
static void console_take(void)
{
    int mode;
    if (ioctl(STDIN_FILENO, KDGETMODE, &mode) < 0) return;
    console_fd = STDIN_FILENO;
    if (ioctl(console_fd, KDGKBMODE, &console_kb) < 0 || console_kb == K_OFF) console_kb = K_UNICODE;
    ioctl(console_fd, KDSETMODE, KD_GRAPHICS);
    ioctl(console_fd, KDSKBMODE, K_OFF);
}
static void on_signal(int sig) { interrupted = sig; }
static void restore(void)
{
    wipe_secrets();
    explicit_bzero(child.input, sizeof child.input);
    console_restore();
    close_fd(&lock_fd);
}
/* Never unlink a held lock: another process could otherwise lock a new inode. */
static int session_lock(void)
{
    struct stat st;
    if (mkdir(RESCUE_RUN_DIR, 0700) < 0 && errno != EEXIST) return -1;
    int dir = open(RESCUE_RUN_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    if (fstat(dir, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
        close(dir);
        return -1;
    }
    int fd = openat(dir, "ui.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }
    lock_fd = fd;
    return 0;
}
static int windowed(void)
{
    const char *backend = getenv("LOCACHY_BACKEND");
    return backend && !strcmp(backend, "wayland");
}
static void take_pointer(void)
{
    if (windowed()) return;
    char paths[32][POINTER_PATH];
    size_t n = 0;
    pointer_open();
    int *fds = pointer_lend(&n, paths, 32);
    for (size_t i = 0; i < n; i++) pointer_take(fds[i], paths[i]);
    free(fds);
}
static void add_log(const char *text)
{
    if (log_count == LOG_MAX) {
        memmove(log_lines, log_lines + 1, sizeof log_lines - sizeof log_lines[0]);
        log_count--;
    }
    snprintf(log_lines[log_count++], sizeof log_lines[0], "%s", text);
}
/* Printable text only. Each call may be a fragment; stderr never shares the
 * JSON pipe. Work per frame is bounded even for an endlessly writing child. */
static void log_bytes(const char *text, size_t size)
{
    size_t at = strlen(partial);
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r' || at + 1 == sizeof partial) {
            partial[at] = '\0';
            add_log(partial);
            at = 0;
            if (c == '\n' || c == '\r') continue;
        }
        partial[at++] = c == '\t' ? ' ' : c < 32 || c == 127 ? '?' : (char)c;
    }
    partial[at] = '\0';
}
static void log_text(const char *text)
{
    log_bytes(text, strlen(text));
    if (partial[0]) { add_log(partial); partial[0] = '\0'; }
}
static struct json_object *field(struct json_object *obj, const char *name, enum json_type type)
{
    struct json_object *v = NULL;
    return obj && json_object_object_get_ex(obj, name, &v) && json_object_is_type(v, type) ? v : NULL;
}
static int string_field(struct json_object *obj, const char *name, char *out, size_t size)
{
    struct json_object *v = field(obj, name, json_type_string);
    if (!v) return 0;
    const char *s = json_object_get_string(v);
    size_t len = (size_t)json_object_get_string_len(v);
    if (len >= size || strlen(s) != len) return 0; /* never truncate an identity */
    for (size_t i = 0; i < len; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return 0;
    memcpy(out, s, len + 1);
    return 1;
}
/* Commit the snapshot atomically. A malformed reply cannot turn an unknown
 * or busy target into a displayed closed one, or authorize a repair. */
static int read_snapshot(struct json_object *reply)
{
    static struct snapshot next;
    memset(&next, 0, sizeof next);
    struct json_object *t = field(reply, "target", json_type_object), *l = field(reply, "link", json_type_object);
    struct json_object *d = field(reply, "devices", json_type_array), *a = field(reply, "interfaces", json_type_array);
    if (!t || !l || !d || !a || !string_field(t, "state", next.state, sizeof next.state)) return 0;
    if (strcmp(next.state, "closed") && strcmp(next.state, "readonly") && strcmp(next.state, "readwrite")) return 0;
#define STR(obj, key, dest) string_field(obj, key, dest, sizeof dest)
    if (!STR(t, "device", next.device) || !STR(t, "uuid", next.uuid) || !STR(t, "mount", next.mount) ||
        !STR(t, "swap", next.swap) || !STR(t, "esp", next.esp) || !STR(l, "interface", next.interface) ||
        !STR(l, "address", next.address) || !STR(l, "fingerprint", next.fingerprint) ||
        !STR(l, "state", next.link_state) || !field(l, "active", json_type_boolean) ||
        !field(l, "port", json_type_int)) return 0;
    struct json_object *optional = NULL;
    if (json_object_object_get_ex(t, "esp_reason", &optional) && !STR(t, "esp_reason", next.esp_reason)) return 0;
    if (json_object_object_get_ex(t, "repair_blocked", &optional)) {
        if (!json_object_is_type(optional, json_type_boolean)) return 0;
        next.repair_blocked = json_object_get_boolean(optional);
    }
    next.active = json_object_get_boolean(field(l, "active", json_type_boolean));
    long long port = json_object_get_int64(field(l, "port", json_type_int));
    if (port < 0 || port > 65535 || (next.active && (!port || !next.address[0] || !next.fingerprint[0]))) return 0;
    next.port = (int)port;
    size_t nd = json_object_array_length(d), ni = json_object_array_length(a);
    if (nd > ITEMS_MAX || ni > ITEMS_MAX) return 0;
    next.ndevices = (int)nd;
    next.ninterfaces = (int)ni;
    for (int i = 0; i < next.ndevices; i++) {
        struct device *v = &next.devices[i];
        struct json_object *o = json_object_array_get_idx(d, (size_t)i);
        if (!STR(o, "path", v->path) || !STR(o, "label", v->label) || !STR(o, "uuid", v->uuid) ||
            !STR(o, "model", v->model) || !field(o, "size", json_type_int) || strncmp(v->path, "/dev/", 5)) return 0;
        v->size = json_object_get_int64(field(o, "size", json_type_int));
        if (v->size < 0) return 0;
    }
    for (int i = 0; i < next.ninterfaces; i++) {
        struct interface *v = &next.interfaces[i];
        struct json_object *o = json_object_array_get_idx(a, (size_t)i);
        if (!STR(o, "name", v->name) || !STR(o, "address", v->address) ||
            !field(o, "carrier", json_type_boolean) || !v->name[0]) return 0;
        v->carrier = json_object_get_boolean(field(o, "carrier", json_type_boolean));
    }
#undef STR
    state = next;
    return 1;
}
static int parse_reply(const char *data, size_t len, int code)
{
    struct json_tokener *tok = json_tokener_new_ex(32);
    if (!tok) return 0;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    struct json_object *reply = json_tokener_parse_ex(tok, data, (int)len);
    size_t end = json_tokener_get_parse_end(tok);
    int parsed = json_tokener_get_error(tok) == json_tokener_success;
    for (; end < len && isspace((unsigned char)data[end]); end++) {}
    parsed = parsed && end == len && reply && json_object_is_type(reply, json_type_object);
    json_tokener_free(tok);
    if (!parsed) { if (reply) json_object_put(reply); return 0; }
    struct json_object *ok = field(reply, "ok", json_type_boolean);
    struct json_object *output = field(reply, "output", json_type_string), *error = field(reply, "error", json_type_string);
    if (output) log_text(json_object_get_string(output));
    if (error && json_object_get_string_len(error)) log_text(json_object_get_string(error));
    known = read_snapshot(reply);
    int success = ok && output && error && known && json_object_get_boolean(ok) && code == 0;
    if (success) {
        tell(0, _("%s completed."), child.command);
    } else if (error && json_object_get_string_len(error)) {
        tell(1, "%s", json_object_get_string(error));
    } else {
        tell(1, _("%s failed; state may be unknown. Inspect the output before continuing."), child.command);
    }
    json_object_put(reply);
    return success;
}
static void stop_child(const char *reason)
{
    if (!child.pid || child.stopping) return;
    log_text(reason);
    tell(1, "%s", reason);
    child.failed = child.stopping = 1;
    child.stop_at = now_ms() + RESCUE_KILL_MS;
    kill(-child.pid, SIGTERM);
    close_fd(&child.in);
    explicit_bzero(child.input, sizeof child.input);
}
static int nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int start_helper(const char *command, const char *argument, const char *password)
{
    if (child.pid) return 0;
    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
    char *reply = malloc(REPLY_MAX + 1);
    if (!reply || pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0 ||
        nonblock(in[1]) < 0 || nonblock(out[0]) < 0 || nonblock(err[0]) < 0) goto fail;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 || dup2(err[1], STDERR_FILENO) < 0) _exit(126);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        if (!strcmp(command, "reboot") || !strcmp(command, "poweroff")) {
            char *const argv[] = { RESCUE_SYSTEMCTL, (char *)command, NULL };
            execv(RESCUE_SYSTEMCTL, argv);
        } else if (!strcmp(command, "keytable")) {
            umask(0077);
            char *const argv[] = { RESCUE_KEYTABLE, (char *)argument, RESCUE_KEYMAP, NULL };
            execv(RESCUE_KEYTABLE, argv);
        } else {
            char *const argv[] = { RESCUE_HELPER, (char *)command, (char *)argument, NULL };
            execv(RESCUE_HELPER, argv);
        }
        _exit(127);
    }
    if (pid < 0) goto fail;
    setpgid(pid, pid);
    close(in[0]); close(out[1]); close(err[1]);
    child = (struct child){ .pid = pid, .in = in[1], .out = out[0], .err = err[0], .reply = reply,
                            .deadline = now_ms() + RESCUE_TIMEOUT_MS,
                            .power = !strcmp(command, "reboot") || !strcmp(command, "poweroff") };
    snprintf(child.command, sizeof child.command, "%s", command);
    child.input_len = password ? strlen(password) : 0;
    if (child.input_len) memcpy(child.input, password, child.input_len);
    else close_fd(&child.in);
    reply[0] = '\0';
    char line[384];
    snprintf(line, sizeof line, "> %s%s%s", command, argument ? " " : "", argument ? argument : "");
    log_text(line);
    output_scroll = 0;
    tell(0, _("Running: %s (close requests stop it, then clean up)."), command);
    return 1;
fail:
    free(reply);
    for (int i = 0; i < 2; i++) { close_fd(&in[i]); close_fd(&out[i]); close_fd(&err[i]); }
    tell(1, _("Cannot start rescue helper: %s"), strerror(errno));
    return 0;
}
static void read_pipe(int *fd, int json)
{
    if (*fd < 0) return;
    struct pollfd p = { .fd = *fd, .events = POLLIN };
    if (poll(&p, 1, 0) <= 0) return;
    char buf[8192];
    for (int reads = 0; reads < 8; reads++) {
        ssize_t n = read(*fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { if (n < 0) child.failed = 1; close_fd(fd); return; }
        if (json && !child.power) {
            if (child.len + (size_t)n > REPLY_MAX) {
                stop_child(_("Rescue helper reply exceeded the size limit; stopping it."));
                close_fd(fd);
                return;
            }
            memcpy(child.reply + child.len, buf, (size_t)n);
            child.len += (size_t)n;
            child.reply[child.len] = '\0';
        } else {
            log_bytes(buf, (size_t)n);
        }
    }
}
static void begin_exit(void);
static void pump_child(void)
{
    if (!child.pid) return;
    double now = now_ms();
    if (child.in >= 0) {
        struct pollfd p = { .fd = child.in, .events = POLLOUT };
        if (poll(&p, 1, 0) > 0) {
            ssize_t n = write(child.in, child.input + child.sent, child.input_len - child.sent);
            if (n > 0) child.sent += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stop_child(_("Rescue helper did not accept the secret; stopping it."));
            }
            if (child.sent == child.input_len) {
                close_fd(&child.in); /* EOF, no added newline */
                explicit_bzero(child.input, sizeof child.input);
            }
        }
    }
    read_pipe(&child.out, 1);
    read_pipe(&child.err, 0);
    if (!child.reaped) {
        pid_t rc = waitpid(child.pid, &child.status, WNOHANG);
        if (rc == child.pid || (rc < 0 && errno == ECHILD)) {
            child.reaped = 1;
            if (rc < 0) child.failed = 1;
            child.drain_at = now + RESCUE_KILL_MS;
        }
    }
    if (!child.terminal && now >= child.deadline && !child.stopping) {
        stop_child(_("Rescue helper timed out; stopping it. State must be checked again."));
    }
    if (child.stopping && now >= child.stop_at) kill(-child.pid, SIGKILL);
    if (child.reaped && (child.out >= 0 || child.err >= 0) && now >= child.drain_at) {
        kill(-child.pid, SIGKILL);
        close_fd(&child.out); close_fd(&child.err);
        child.failed = 1;
        log_text(_("A helper descendant kept its output open; the operation was not verified."));
    }
    if (!child.reaped || child.out >= 0 || child.err >= 0) return;
    if (partial[0]) { add_log(partial); partial[0] = '\0'; }
    int cleanup = !strcmp(child.command, "cleanup"), terminal = child.terminal;
    int keytable = !strcmp(child.command, "keytable");
    int code = WIFEXITED(child.status) ? WEXITSTATUS(child.status) : -1;
    int success = 0;
    if (child.power) {
        if (code != 0 || child.failed) {
            tell(1, _("%s failed (status %d); the closed rescue session remains on screen."), child.command, code);
        } else {
            tell(0, "%s", _("Shutdown requested. Waiting for the live system."));
        }
        log_text(notice);
    } else if (keytable) {
        if (!child.failed && code == 0 && keys_layout_load(RESCUE_KEYMAP) == 0) {
            snprintf(keyboard, sizeof keyboard, "%s", next_keyboard);
            own_keyboard = 1;
            tell(0, _("Rescue keyboard layout: %s. F2 can show a passphrase to check it."), keyboard);
        } else {
            tell(1, "%s", _("Keyboard layout was not changed; check keytable output."));
        }
    } else if (terminal) {
        if (handed_screen) {
            console_take();
            screen_open(BASE_FONT); keys_open(); take_pointer();
            handed_screen = 0;
        }
        if (code != 0 || child.failed) {
            tell(1, _("Target terminal failed (status %d); Rescue remains open."), code);
            log_text(notice);
        }
    } else {
        known = 0;
        notice_error = 0;
        if (!child.failed && child.reply && child.len) success = parse_reply(child.reply, child.len, code);
        if (!success && !notice_error) {
            tell(1, _("%s failed (status %d); state is not verified. Inspect the output."), child.command, code);
        }
    }
    if (child.stopping) kill(-child.pid, SIGKILL);
    close_fd(&child.in);
    explicit_bzero(child.input, sizeof child.input);
    free(child.reply);
    child = (struct child){ .in = -1, .out = -1, .err = -1 };
    if (cleanup && exiting) {
        if (success && !strcmp(state.state, "closed") && !state.active) {
            say("rescue: session cleanup verified");
            if (boot_session && !signal_exit) {
                session_closed = 1;
                exiting = closed_selected = power_confirmation = 0;
                wipe_secrets();
                textsel_clear();
                tell(0, "%s", _("All rescue resources are closed. Choose what to do next."));
            } else {
                done = 1;
            }
        } else {
            exiting = 0;
            tell(1, "%s", _("Cleanup did not finish; Rescue remains open. Release busy resources and retry Exit."));
            log_text(notice);
            key_close_kept();
        }
    } else if (exiting) {
        if (!start_helper("cleanup", NULL, NULL)) { exiting = 0; key_close_kept(); }
    } else if (terminal && code == 0) {
        start_helper("status", NULL, NULL);
    }
}
static void begin_exit(void)
{
    wipe_secrets();
    modal = M_NONE;
    textsel_clear();
    key_close_kept();
    if (exiting) return;
    if (session_closed && !child.pid) {
        if (signal_exit) done = 1;
        return;
    }
    exiting = 1;
    if (child.pid) {
        stop_child(_("Stopping the current operation before session cleanup."));
    } else if (!start_helper("cleanup", NULL, NULL)) {
        exiting = 0;
    }
}
static void terminal(void)
{
    if (!windowed()) {
        pointer_release(); keys_release(); screen_release();
        handed_screen = 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        char *const argv[] = { RESCUE_TERM, RESCUE_HELPER, "target-terminal", NULL };
        if (own_keyboard) setenv("LOCACHY_KEYMAP_TABLE", RESCUE_KEYMAP, 1);
        else unsetenv("LOCACHY_KEYMAP_TABLE");
        execv(RESCUE_TERM, argv);
        _exit(127);
    }
    if (pid < 0) {
        if (handed_screen) { console_take(); screen_open(BASE_FONT); keys_open(); take_pointer(); handed_screen = 0; }
        tell(1, _("Cannot start target terminal: %s"), strerror(errno));
        return;
    }
    setpgid(pid, pid);
    child = (struct child){ .pid = pid, .in = -1, .out = -1, .err = -1, .terminal = 1 };
    snprintf(child.command, sizeof child.command, "target-terminal");
    tell(0, "%s", _("Target terminal is open. Exit it to return to Rescue."));
    log_text(notice);
}
/* UTF-8 boundaries are preserved both while editing and while wrapping. */
static size_t next_char(const char *s, size_t at)
{
    if (!s[at]) return at;
    at++;
    while (((unsigned char)s[at] & 0xC0) == 0x80) at++;
    return at;
}
static size_t prev_char(const char *s, size_t at)
{
    if (!at) return 0;
    at--;
    while (at && ((unsigned char)s[at] & 0xC0) == 0x80) at--;
    return at;
}
static size_t characters(const char *s)
{
    size_t n = 0;
    for (size_t at = 0; s[at]; at = next_char(s, at)) n++;
    return n;
}
static int edit(int key, char *text, size_t size, size_t *pos)
{
    size_t len = strlen(text);
    if (*pos > len) *pos = len;
    if (key == K_LEFT) *pos = prev_char(text, *pos);
    else if (key == K_RIGHT) *pos = next_char(text, *pos);
    else if (key == K_BOL || key == 1) *pos = 0;
    else if (key == K_EOL || key == 5) *pos = len;
    else if (key == '\b' || key == 127) {
        size_t p = prev_char(text, *pos);
        memmove(text + p, text + *pos, len - *pos + 1);
        *pos = p;
    } else if (key == K_DEL || key == 4) {
        size_t p = next_char(text, *pos);
        memmove(text + *pos, text + p, len - p + 1);
    } else if (key == 21) {
        memmove(text, text + *pos, len - *pos + 1);
        *pos = 0;
    } else if (key == 11) {
        text[*pos] = '\0';
    } else if (key == 23) {
        size_t p = *pos;
        while (p && text[p - 1] == ' ') p = prev_char(text, p);
        while (p && text[p - 1] != ' ') p = prev_char(text, p);
        memmove(text + p, text + *pos, len - *pos + 1);
        *pos = p;
    } else {
        const char *typed = keys_typed(key);
        if (!typed) return 0;
        size_t n = strlen(typed);
        if (len + n >= size) { tell(1, "%s", _("Input is too long; nothing was added.")); return 0; }
        memmove(text + *pos + n, text + *pos, len - *pos + 1);
        memcpy(text + *pos, typed, n);
        *pos += n;
    }
    return 1;
}
static void secret_prompt(int which)
{
    wipe_secrets();
    textsel_clear();
    modal = which;
    tell(0, "%s", _("Secrets stay in this UI and the helper's stdin; never in argv or files."));
}
static void do_action(int which)
{
    if (which == A_EXIT) { begin_exit(); return; }
    if (child.pid) { tell(1, "%s", _("Wait for the current operation, or Exit to stop it and clean up.")); return; }
    if (which == A_SCAN || which == A_STATUS) { start_helper(which == A_SCAN ? "scan" : "status", NULL, NULL); return; }
    if (which == A_KEYMAP) {
        if (windowed()) {
            tell(0, "%s", _("In a Wayland window, change the keyboard layout in desktop settings; Rescue uses that window's keys."));
        } else {
            modal = M_LAYOUT; filter[0] = '\0'; textsel_clear();
        }
        return;
    }
    if (!known) { tell(1, "%s", _("State is not verified. Rescan or inspect status first.")); return; }
    switch (which) {
    case A_UNLOCK:
        if (strcmp(state.state, "closed")) tell(1, "%s", _("Close the current target before selecting another."));
        else if (!state.ndevices) tell(1, "%s", _("No encrypted target was found. Rescan after attaching its disk."));
        else { modal = M_DEVICE; pick_sel = pick_top = 0; filter[0] = '\0'; textsel_clear(); }
        break;
    case A_REPAIR:
        if (state.repair_blocked) tell(1, _("Repair is blocked: %s"), state.esp_reason[0] ? state.esp_reason : _("the target ESP is not uniquely identified"));
        else if (strcmp(state.state, "readonly")) tell(1, "%s", _("First unlock and inspect the target read-only."));
        else { modal = M_CONFIRM; textsel_clear(); }
        break;
    case A_CLOSE: start_helper("close-target", NULL, NULL); break;
    case A_START_LINK:
        if (state.active) tell(1, "%s", _("The debug link is already active. Stop it before selecting another interface."));
        else if (!state.ninterfaces) tell(1, "%s", _("No eligible wired interface was found. Rescan after attaching one."));
        else { modal = M_INTERFACE; pick_sel = pick_top = 0; filter[0] = '\0'; textsel_clear(); }
        break;
    case A_STOP_LINK: start_helper("stop-link", NULL, NULL); break;
    case A_TERMINAL:
        if (strcmp(state.state, "readwrite")) tell(1, "%s", _("Target terminal requires explicitly enabled read/write repair."));
        else terminal();
        break;
    default: break;
    }
}
static void submit_secret(void)
{
    if (!secret[0]) { tell(1, "%s", _("A nonempty secret is required.")); return; }
    if (modal == M_LINK) {
        if (characters(secret) < 12) { tell(1, "%s", _("The debug-link password needs at least 12 characters.")); return; }
        memcpy(first_secret, secret, sizeof secret);
        explicit_bzero(secret, sizeof secret);
        secret_pos = 0; reveal = 0; modal = M_LINK_AGAIN;
        return;
    }
    if (modal == M_LINK_AGAIN && strcmp(secret, first_secret)) {
        wipe_secrets(); modal = M_LINK;
        tell(1, "%s", _("The passwords did not match. Set the debug-link password again."));
        return;
    }
    const char *command = modal == M_UNLOCK ? "unlock-readonly" : modal == M_REPAIR ? "enable-repair" : "start-link";
    const char *arg = modal == M_REPAIR ? "--confirm" : pending;
    start_helper(command, arg, secret);
    wipe_secrets(); modal = M_NONE;
}
static void human_size(long long bytes, char *out, size_t size)
{
    snprintf(out, size, "%.1f GiB (%lld B)", (double)bytes / 1073741824, bytes);
}
static void device_text(int index, char *out, size_t size)
{
    const struct device *d = &state.devices[index];
    char sz[64]; human_size(d->size, sz, sizeof sz);
    snprintf(out, size, "%s | %s | %s | %s | UUID %s", d->path, sz, d->label, d->model, d->uuid);
}
static void interface_text(int index, char *out, size_t size)
{
    const struct interface *v = &state.interfaces[index];
    snprintf(out, size, "%s | %s | %s", v->name, v->address, v->carrier ? _("carrier present") : _("no carrier"));
}
static int matches(int match[ITEMS_MAX])
{
    int count = 0, total = modal == M_DEVICE ? state.ndevices : state.ninterfaces;
    char line[LINE_MAXIMUM];
    for (int i = 0; i < total; i++) {
        if (modal == M_DEVICE) device_text(i, line, sizeof line); else interface_text(i, line, sizeof line);
        if (!filter[0] || strcasestr(line, filter)) match[count++] = i;
    }
    return count;
}
static void choose_pick(void)
{
    int match[ITEMS_MAX], n = matches(match);
    if (pick_sel < 0 || pick_sel >= n) return;
    int which = modal, index = match[pick_sel];
    snprintf(pending, sizeof pending, "%s", which == M_DEVICE ? state.devices[index].path : state.interfaces[index].name);
    secret_prompt(which == M_DEVICE ? M_UNLOCK : M_LINK);
}
/* Wrap text at character boundaries, keeping long UUIDs, addresses and output
 * reachable on narrow screens instead of silently clipping their ends. */
static size_t cut_cells(const char *s, int cells)
{
    size_t at = 0;
    int used = 0;
    while (s[at]) {
        size_t next = next_char(s, at);
        char ch[8] = "";
        if (next - at >= sizeof ch) break;
        memcpy(ch, s + at, next - at);
        int w = ui_cells(ch);
        if (used + w > cells && at) break;
        at = next; used += w;
        if (used >= cells) break;
    }
    return at;
}
static void status_line(int width, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status_line(int width, const char *fmt, ...)
{
    char line[2048]; va_list ap;
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    const char *s = line;
    do {
        size_t n = cut_cells(s, width);
        if (nstatus == 128) return;
        snprintf(status_lines[nstatus++], sizeof status_lines[0], "%.*s", (int)n, s);
        s += n;
    } while (*s);
}
static void build_status(int width)
{
    nstatus = 0; ssh[0] = '\0';
    status_line(width, _("Keyboard: %s (Rescue and its target terminal; system settings are unchanged)"), keyboard);
    status_line(width, _("Target: %s%s"), state.state[0] ? state.state : _("unknown"), known ? "" : _(" (not verified)"));
    status_line(width, _("Device: %s   UUID: %s"), state.device[0] ? state.device : "-", state.uuid[0] ? state.uuid : "-");
    for (int i = 0; i < state.ndevices; i++) {
        if (strcmp(state.devices[i].path, state.device)) continue;
        char sz[64]; human_size(state.devices[i].size, sz, sizeof sz);
        status_line(width, _("Identity: %s | %s | %s"), state.devices[i].label, state.devices[i].model, sz);
    }
    status_line(width, _("Mount: %s   Swap: %s   ESP: %s"), state.mount[0] ? state.mount : "-",
                state.swap[0] ? state.swap : "-", state.esp[0] ? state.esp : "-");
    if (state.esp_reason[0]) status_line(width, _("ESP note: %s"), state.esp_reason);
    if (state.repair_blocked) status_line(width, _("Repair blocked: %s"), state.esp_reason[0] ? state.esp_reason : _("the target ESP is not uniquely identified"));
    status_line(width, _("Debug link: %s | %s | %s | %s"), state.active ? _("active") : _("off"),
                state.link_state, state.interface, state.address);
    status_line(width, _("Host key: %s"), state.fingerprint[0] ? state.fingerprint : "-");
    if (state.active) {
        char host[512];
        size_t address_len = strcspn(state.address, "%");
        snprintf(host, sizeof host, "%.*s%%<local-interface>", (int)address_len, state.address);
        snprintf(ssh, sizeof ssh, "ssh -S none -p %d root@%s", state.port, host);
        status_line(width, "%s", ssh);
        status_line(width, _("SFTP: sftp -P %d root@[%s]"), state.port, host);
        status_line(width, "%s", _("Replace <local-interface> with the connecting computer's interface. Verify the host-key fingerprint; never auto-accept it."));
    }
    status_line(width, _("Found: %d encrypted targets, %d wired interfaces. No automatic unlock, TPM, fsck or formatting."),
                state.ndevices, state.ninterfaces);
}
static int clamp(int value, int max)
{
    return value < 0 ? 0 : value > max ? (max > 0 ? max : 0) : value;
}
static void layout(void)
{
    int cols = screen_cols(), rows = screen_rows();
    status_h = rows / 3;
    if (status_h > 12) status_h = 12;
    if (status_h < 3) status_h = 3;
    list_y = status_h + 3;
    int available = rows - list_y - 3;
    if (available < 4) available = 4;
    if (cols >= 75) {
        action_w = cols * 2 / 5;
        if (action_w > 38) action_w = 38;
        list_h = available - 1;
        output_x = action_w + 1; output_y = list_y; output_h = available;
    } else {
        action_w = cols;
        list_h = available / 3;
        if (list_h < 2) list_h = 2;
        output_x = 0; output_y = list_y + list_h + 1; output_h = available - list_h - 1;
    }
}
static void draw_output(void)
{
    int width = screen_cols() - output_x - 3, body = output_h - 1;
    if (width < 1 || body < 1) return;
    /* Map wrapped visual rows without allocating a second copy of the log. */
    static struct { int line; size_t offset, size; } rows[LOG_MAX * 64];
    int total = 0;
    for (int i = 0; i < log_count; i++) {
        const char *s = log_lines[i]; size_t offset = 0;
        do {
            size_t n = cut_cells(s + offset, width);
            if (total == (int)(sizeof rows / sizeof rows[0])) break;
            rows[total].line = i; rows[total].offset = offset; rows[total++].size = n;
            offset += n;
        } while (s[offset]);
    }
    output_scroll = clamp(output_scroll, total - body);
    int first = total - body - output_scroll;
    if (first < 0) first = 0;
    char line[LINE_MAXIMUM];
    screen_fill(output_x, output_y, screen_cols() - output_x, output_h, PANE);
    screen_fill(output_x, output_y, screen_cols() - output_x, 1, focus == F_OUTPUT ? ACCENT : BAR);
    snprintf(line, sizeof line, _("Output: %d-%d / %d%s"), total ? first + 1 : 0,
             first + body < total ? first + body : total, total, child.pid ? _(" (running)") : "");
    textsel_put(output_x + 1, output_y, line, focus == F_OUTPUT ? BG : FG, focus == F_OUTPUT ? ACCENT : BAR);
    ui_hit(output_x, output_y, screen_cols() - output_x, output_h, HIT_OUTPUT);
    for (int r = 0; r < body && first + r < total; r++) {
        int k = first + r;
        snprintf(line, sizeof line, "%.*s", (int)rows[k].size, log_lines[rows[k].line] + rows[k].offset);
        textsel_put(output_x + 1, output_y + 1 + r, line, FG, PANE);
    }
}
static void draw_modal(void)
{
    char line[LINE_MAXIMUM];
    int cols = screen_cols(), rows = screen_rows();
    screen_fill(0, 1, cols, rows - 2, BG);
    ui_hits_clear(); textsel_frame();
    if (modal == M_DEVICE || modal == M_INTERFACE) {
        int match[ITEMS_MAX], n = matches(match), height = rows - 8;
        if (height < 1) height = 1;
        pick_sel = clamp(pick_sel, n - 1);
        if (pick_sel < pick_top) pick_top = pick_sel;
        if (pick_sel >= pick_top + height) pick_top = pick_sel - height + 1;
        screen_text(2, 2, modal == M_DEVICE ? _("Choose the encrypted target (read-only unlock)") : _("Choose the dedicated wired debug interface"), ACCENT, BG);
        snprintf(line, sizeof line, _("Filter: %s_   %d matches"), filter, n);
        screen_text(2, 3, line, FG, BG);
        for (int i = 0; i < height && pick_top + i < n; i++) {
            int k = pick_top + i, on = k == pick_sel;
            if (modal == M_DEVICE) device_text(match[k], line, sizeof line); else interface_text(match[k], line, sizeof line);
            screen_fill(1, 5 + i, cols - 2, 1, on ? ACCENT : BG);
            size_t from = 0;
            for (int offset = 0; line[from] && offset < pick_offset; offset++) from = next_char(line, from);
            screen_text(2, 5 + i, line + from, on ? BG : FG, on ?ACCENT : BG);
            ui_hit(1, 5 + i, cols - 2, 1, HIT_PICK + k);
        }
        int x = ui_button(1, rows - 3, _("Enter: choose"), HIT_KEY + '\n');
        ui_button(x, rows - 3, _("Esc: cancel"), HIT_KEY + 27);
    } else if (modal == M_LAYOUT) {
        screen_text(2, 2, _("Keyboard layout (only this Rescue UI)"), ACCENT, BG);
        screen_text(2, 4, _("Choose US or Japanese JIS, or type a standard console/XKB layout name."), FG, BG);
        snprintf(line, sizeof line, _("Layout: %s_"), filter);
        screen_text(2, 6, line, FG, BG);
        int x = ui_button(1, rows - 4, _("US"), HIT_KEY + K_F1 + 1);
        ui_button(x, rows - 4, _("Japanese JIS"), HIT_KEY + K_F1 + 2);
        x = ui_button(1, rows - 3, _("Enter: apply"), HIT_KEY + '\n');
        ui_button(x, rows - 3, _("Esc: cancel"), HIT_KEY + 27);
    } else if (modal == M_CONFIRM) {
        screen_text(2, 2, _("Enable write repairs on this target?"), ERROR, BG);
        snprintf(line, sizeof line, "%s | UUID %s", state.device, state.uuid);
        screen_text(2, 4, line, FG, BG);
        screen_text(2, 6, _("The read-only target will close and reopen read/write."), FG, BG);
        screen_text(2, 7, _("The next screen asks for the disk passphrase again. Y explicitly authorizes writes."), ERROR, BG);
        int x = ui_button(1, rows - 3, _("Y: enable repair"), HIT_KEY + 'y');
        ui_button(x, rows - 3, _("N: cancel"), HIT_KEY + 27);
    } else {
        const char *label = modal == M_UNLOCK ? _("Disk passphrase (read-only unlock)") : modal == M_REPAIR ?
                            _("Disk passphrase (confirmed write repair)") : modal == M_LINK ?
                            _("New debug-link password (at least 12 characters)") : _("Repeat the debug-link password");
        screen_text(2, 2, label, ACCENT, BG);
        screen_text(2, 3, modal == M_REPAIR ? state.device : pending, DIM, BG);
        snprintf(line, sizeof line, _("Keyboard: %s   F2 shows/hides the input so you can check the layout."), keyboard);
        screen_text(2, 4, line, DIM, BG);
        char shown[SECRET_MAX + 1];
        size_t cursor;
        if (reveal) { memcpy(shown, secret, sizeof shown); cursor = secret_pos; }
        else { cursor = 0; size_t n = characters(secret); memset(shown, '*', n); shown[n] = '\0';
               for (size_t at = 0; at < secret_pos; at = next_char(secret, at)) cursor++; }
        /* Keep the cursor visible without ever giving secret text to textsel. */
        size_t from = 0;
        while (from < cursor && ui_cells(shown + from) > cols - 5) from = next_char(shown, from);
        snprintf(line, sizeof line, "%.*s_%s", (int)(cursor - from), shown + from, shown + cursor);
        screen_text(2, 5, line, FG, BG);
        explicit_bzero(shown, sizeof shown); explicit_bzero(line, sizeof line);
        int x = ui_button(1, rows - 3, _("Enter: submit"), HIT_KEY + '\n');
        x = ui_button(x, rows - 3, _("Esc: cancel"), HIT_KEY + 27);
        ui_button(x, rows - 3, reveal ? _("F2: hide") : _("F2: show"), HIT_KEY + K_F1 + 1);
    }
}
static void draw_closed(void)
{
    int cols = screen_cols(), rows = screen_rows();
    screen_text(2, 2, _("Rescue session closed"), ACCENT, BG);
    screen_text(2, 4, _("The debug link is stopped and target filesystems are unmounted."), FG, BG);
    screen_text(2, 5, _("No terminal or installer will open automatically."), DIM, BG);
    if (power_confirmation) {
        screen_text(2, 7, power_confirmation == 1 ? _("Reboot this computer?") : _("Power off this computer?"), ERROR, BG);
        int x = ui_button(2, 9, _("Y: confirm"), HIT_KEY + 'y');
        ui_button(x, 9, _("N / Esc: cancel"), HIT_KEY + 'n');
    } else {
        const char *items[] = { _("Start a new rescue session"), _("Reboot"), _("Power off") };
        for (int i = 0; i < 3; i++) {
            int on = closed_selected == i;
            screen_fill(1, 7 + i * 2, cols > 2 ? cols - 2 : 0, 1, on ? BAR : BG);
            screen_text(2, 7 + i * 2, items[i], on ? ACCENT : FG, on ? BAR : BG);
            ui_hit(1, 7 + i * 2, cols > 2 ? cols - 2 : 0, 1, HIT_ACTION + i);
        }
        screen_text(2, rows - 3, _("Up/Down: choose   Enter: open   r: new session   b: reboot   p: power off"), DIM, BG);
    }
}
static void draw(void)
{
    if (handed_screen) return;
    int cols = screen_cols(), rows = screen_rows();
    ui_hits_clear(); textsel_frame();
    screen_fill(0, 0, cols, rows, BG);
    if (rows < 16 || cols < 24) {
        screen_text(1, 2, _("Enlarge the Rescue window. Q exits with cleanup."), ERROR, BG);
    } else if (session_closed) {
        draw_closed();
    } else if (modal != M_NONE) {
        draw_modal();
    } else {
        layout(); build_status(cols - 4);
        screen_fill(0, 1, cols, 1, focus == F_STATUS ? ACCENT : BAR);
        screen_text(1, 1, _("Rescue status (Tab: change pane; arrows / wheel: scroll)"), focus == F_STATUS ? BG : FG,
                    focus == F_STATUS ? ACCENT : BAR);
        status_scroll = clamp(status_scroll, nstatus - status_h);
        ui_hit(0, 1, cols, status_h + 1, HIT_STATUS);
        for (int i = 0; i < status_h && status_scroll + i < nstatus; i++)
            textsel_put(2, 2 + i, status_lines[status_scroll + i], known ? FG : ERROR, BG);
        if (selected < action_top) action_top = selected;
        if (selected >= action_top + list_h) action_top = selected - list_h + 1;
        screen_fill(0, list_y, action_w, 1, focus == F_ACTIONS ? ACCENT : BAR);
        screen_text(1, list_y, _("Actions (Enter: run)"), focus == F_ACTIONS ? BG : FG, focus == F_ACTIONS ? ACCENT : BAR);
        char action_line[LINE_MAXIMUM];
        for (int i = 0; i < list_h && action_top + i < NACTIONS; i++) {
            int k = action_top + i, on = selected == k && focus == F_ACTIONS;
            screen_fill(0, list_y + 1 + i, action_w, 1, on ? BAR : BG);
            snprintf(action_line, sizeof action_line, "%s%s", _(actions[k]),
                     k == A_EXIT && (child.pid || exiting) ? _(" (cleanup required)") : "");
            screen_text(1, list_y + 1 + i, action_line, on ? ACCENT : FG, on ? BAR : BG);
            ui_hit(0, list_y + 1 + i, action_w, 1, HIT_ACTION + k);
        }
        draw_output(); textsel_draw(BG, ACCENT);
        screen_text(1, rows - 3, _("Tab: pane  PgUp/PgDn: output  y/Ctrl+C: copy  a: SSH  f: fingerprint  q: exit"), DIM, BG);
    }
    screen_text(1, rows - 1, notice, notice_error ? ERROR : ACCENT, BG);
    screen_bar(_("Rescue"), NULL);
    screen_flush();
}
static void copy_value(int primary, const char *text)
{
    char msg[512];
    int error = textsel_copy(primary, text, msg, sizeof msg);
    if (msg[0]) tell(error, "%s", msg);
}
static void copy_key(int key)
{
    if (key == 'a' && known && state.active && ssh[0]) copy_value(0, ssh);
    else if (key == 'f' && known && state.fingerprint[0]) copy_value(0, state.fingerprint);
    else {
        char *text = textsel_get();
        if (text && text[0]) copy_value(0, text);
        else tell(0, "%s", _("Select status or output text with the pointer; a copies SSH, f copies the fingerprint."));
        free(text);
    }
}
static int pointer_key(void)
{
    const struct pointer_event *e = pointer_last();
    int hit = ui_hit_at(e->col, e->row);
    if (e->wheel) {
        if (session_closed) closed_selected = clamp(closed_selected - e->wheel, 2);
        else if (modal == M_DEVICE || modal == M_INTERFACE) pick_sel -= e->wheel;
        else if (!modal && hit == HIT_STATUS) status_scroll = clamp(status_scroll - e->wheel * 3, nstatus - status_h);
        else if (!modal && hit == HIT_OUTPUT) output_scroll = clamp(output_scroll + e->wheel * 3, LOG_MAX * 64);
        else if (!modal) selected = clamp(selected - e->wheel, NACTIONS - 1);
        return 0;
    }
    if (!modal && !session_closed) {
        int got = textsel_pointer(e, hit == HIT_STATUS || hit == HIT_OUTPUT);
        if (got == TEXTSEL_MADE) {
            char *text = textsel_get(); if (text && text[0]) copy_value(1, text); free(text);
        }
        if (got != TEXTSEL_NONE) return 0;
    }
    if (!e->pressed || e->button != BTN_LEFT) return 0;
    if (hit >= HIT_PICK && (modal == M_DEVICE || modal == M_INTERFACE)) {
        pick_sel = hit - HIT_PICK;
        return e->clicks >= 2 ? '\n' : 0;
    }
    if (hit >= HIT_KEY && hit < HIT_STATUS) return hit - HIT_KEY;
    if (session_closed && hit >= HIT_ACTION && hit < HIT_ACTION + 3) {
        closed_selected = hit - HIT_ACTION; return '\n';
    }
    if (!modal && hit >= HIT_ACTION && hit < HIT_ACTION + NACTIONS) {
        selected = hit - HIT_ACTION; focus = F_ACTIONS; return '\n';
    }
    if (hit == HIT_STATUS) focus = F_STATUS;
    if (hit == HIT_OUTPUT) focus = F_OUTPUT;
    return 0;
}
static void handle_closed(int key)
{
    if (child.pid) return;
    if (power_confirmation) {
        int requested = power_confirmation;
        power_confirmation = 0;
        if (key == 'y' || key == 'Y') {
            start_helper(requested == 1 ? "reboot" : "poweroff", NULL, NULL);
        } else {
            tell(0, "%s", _("Cancelled; the rescue session remains closed."));
        }
        return;
    }
    if (key == K_UP || key == K_DOWN || key == K_BOL || key == K_EOL) {
        closed_selected = key == K_BOL ? 0 : key == K_EOL ? 2 :
                          clamp(closed_selected + (key == K_UP ? -1 : 1), 2);
        return;
    }
    if (key == 'r') closed_selected = 0;
    else if (key == 'b') closed_selected = 1;
    else if (key == 'p') closed_selected = 2;
    else if (key != '\n' && key != ' ') return;
    if (closed_selected) {
        power_confirmation = closed_selected;
        textsel_clear();
    } else {
        session_closed = 0;
        known = selected = action_top = focus = status_scroll = output_scroll = 0;
        log_count = 0;
        partial[0] = notice[0] = '\0';
        say("rescue: new session");
        start_helper("status", NULL, NULL);
    }
}
static void handle(int key)
{
    if (key == K_CLOSE) { begin_exit(); return; }
    if (key == K_POINTER) key = pointer_key();
    if (!key || key == K_SHOWN) return;
    if (exiting) return;
    if (session_closed) { handle_closed(key); return; }
    if (modal) {
        if (key == 27 || (modal == M_CONFIRM && (key == 'n' || key == 'N'))) {
            wipe_secrets(); modal = M_NONE; tell(0, "%s", _("Cancelled; nothing was requested.")); return;
        }
        if (modal == M_CONFIRM) { if (key == 'y' || key == 'Y') secret_prompt(M_REPAIR); return; }
        if (modal == M_LAYOUT) {
            if (key == K_F1 + 1 || key == K_F1 + 2) {
                snprintf(filter, sizeof filter, "%s", key == K_F1 + 1 ? "us" : "jp");
                key = '\n';
            }
            if (key == '\n') {
                if (!filter[0] || strspn(filter, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-,") != strlen(filter)) {
                    tell(1, "%s", _("Enter a standard console or XKB layout name."));
                } else {
                    snprintf(next_keyboard, sizeof next_keyboard, "%s", filter);
                    unlink(RESCUE_KEYMAP_TMP);
                    start_helper("keytable", next_keyboard, NULL);
                    modal = M_NONE;
                }
            } else { size_t pos = strlen(filter); edit(key, filter, sizeof filter, &pos); }
            return;
        }
        if (modal == M_DEVICE || modal == M_INTERFACE) {
            if (key == '\n') choose_pick();
            else if (key == K_UP) pick_sel--;
            else if (key == K_DOWN) pick_sel++;
            else if (key == K_PGUP) pick_sel -= screen_rows() - 8;
            else if (key == K_PGDN) pick_sel += screen_rows() - 8;
            else if (key == K_BOL) pick_sel = 0;
            else if (key == K_EOL) pick_sel = ITEMS_MAX;
            else if (key == K_LEFT) pick_offset = clamp(pick_offset - 8, LINE_MAXIMUM);
            else if (key == K_RIGHT) pick_offset = clamp(pick_offset + 8, LINE_MAXIMUM);
            else { size_t pos = strlen(filter); if (edit(key, filter, sizeof filter, &pos)) pick_sel = pick_top = 0; }
        } else if (key == '\n') submit_secret();
        else if (key == K_F1 + 1) reveal = !reveal;
        else edit(key, secret, sizeof secret, &secret_pos);
        return;
    }
    if (key == 'q' || key == 27) { begin_exit(); return; }
    if (key == 'y' || key == 3 || key == 'a' || key == 'f') { copy_key(key); return; }
    if (key == '\t') { focus = (focus + 1) % 3; return; }
    if (key == K_PGUP || key == K_PGDN) {
        output_scroll = clamp(output_scroll + (key == K_PGUP ? 8 : -8), LOG_MAX * 64); return;
    }
    if (key == K_UP || key == K_DOWN || key == K_BOL || key == K_EOL) {
        int delta = key == K_UP ? -1 : 1;
        if (focus == F_ACTIONS) selected = key == K_BOL ? 0 : key == K_EOL ? NACTIONS - 1 : clamp(selected + delta, NACTIONS - 1);
        else if (focus == F_STATUS) status_scroll = key == K_BOL ? 0 : key == K_EOL ? nstatus : clamp(status_scroll + delta, nstatus - status_h);
        else output_scroll = key == K_BOL ? LOG_MAX * 64 : key == K_EOL ? 0 : clamp(output_scroll - delta, LOG_MAX * 64);
    } else if ((key == '\n' || key == ' ') && focus == F_ACTIONS) do_action(selected);
}
int main(void)
{
    struct rlimit core = { 0, 0 };
    if (setrlimit(RLIMIT_CORE, &core) < 0) return 1;
    const char *boot = getenv("LOCACHY_RESCUE_BOOT");
    boot_session = boot && !strcmp(boot, "1");
    if (session_lock() < 0) {
        fprintf(stderr, "rescue: cannot lock the private UI session (another Rescue may be running)\n");
        return 1;
    }
    atexit(restore);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal); signal(SIGQUIT, on_signal);
    base_language_reload();
    console_take();
    if (!screen_open(BASE_FONT)) {
        fprintf(stderr, "rescue: no screen to use\n");
        restore(); return 1;
    }
    keys_leds(1); keys_open(); take_pointer();
    const char *layout_name = keys_layout_name("console");
    snprintf(keyboard, sizeof keyboard, "%s", windowed() ? _("desktop window layout") : layout_name ? layout_name : "us");
    say("rescue: started");
    start_helper("status", NULL, NULL);
    while (!done) {
        draw();
        /* The helper's stdout can wake input polling; stderr and stdin use
         * zero-time poll in pump_child, at most 50 ms later. Wayland events
         * are always dispatched by the base, including while a helper runs. */
        int key = key_poll_timeout(child.out, child.pid ? 50 : 500);
        if (interrupted) { interrupted = 0; signal_exit = 1; begin_exit(); }
        pump_child();
        handle(key);
    }
    wipe_secrets(); textsel_clear();
    pointer_release(); keys_release(); screen_release();
    keys_leds(0); restore();
    return 0;
}
