/*
 * Terminal transport for LOCachy programs (SSH / PTY).
 *
 * When a program runs interactively on a PTY with no process-1 control socket
 * (e.g. over SSH while extdisplay owns the local DRM display), this backend
 * probes the remote terminal in-band (working cleanly even through sudo) and
 * chooses the highest-fidelity transport supported:
 *
 *   1. Kitty Graphics Protocol (Ghostty, Kitty, WezTerm, Konsole):
 *      Reuses screen.c's exact XRGB8888 software framebuffer (shadow +
 *      present_diff) via dirty horizontal cell-row strips with ping-pong image
 *      IDs, zlib compression (when built with CONFIG_ZLIB), and Synchronized
 *      Output (DEC 2026). Retina/HiDPI scale is inferred from ws_ypixel/ws_row.
 *   2. Sixel Graphics (iTerm2, foot, Windows Terminal, xterm):
 *      Also reuses screen.c's exact XRGB8888 software framebuffer via adaptive
 *      palette (exact up to 256 colors, 6x6x6 cube + greyscale fallback) and
 *      RLE sixel bands wrapped in DEC 2026.
 *   3. ANSI Cell Fallback (macOS Terminal.app, tmux without passthrough, plain
 *      SSH):
 *      Diff-rendered UTF-8 cell grid (matching screen.c's CJK width table) with
 *      24-bit TrueColor or nearest xterm-256 SGR colors.
 *
 * Keyboard decoding handles Kitty keyboard protocol (CSI u), xterm CSI/SS3,
 * modifyOtherKeys, UTF-8, Esc disambiguation (40 ms), Ctrl/Alt/Shift modifiers,
 * and SGR pixel (1016) / cell (1006) mouse events fed into pointer_window_*.
 */
#ifdef CONFIG_TTY
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef CONFIG_ZLIB
#include <zlib.h>
#endif

#include "internal.h"
#include "include/tty-backend.h"

/* Weak hooks into pointer.c and control.c so standalone unit tests linking
 * tty-backend.o alone compile cleanly. */
__attribute__((weak)) int control_fd(void);
__attribute__((weak)) void pointer_window_motion(int x, int y);
__attribute__((weak)) void pointer_window_button(int code, int pressed);
__attribute__((weak)) void pointer_window_scroll(int source, double v, double h, int axes);
__attribute__((weak)) void pointer_window_present(int on);
__attribute__((weak)) void pointer_window_leave(void);

#define MAX_STRIP_ROWS 512
#define KEY_QUEUE_CAP  128
#define IN_BUF_CAP     8192
#define ESC_TIMEOUT_MS 40
#define PROBE_TIMEOUT_MS 150
#define OSC52_MAX_BYTES (64 * 1024)

struct tty_key_ev {
    int key;
    int mods;
    char text[64];
};

struct tty_cell {
    char utf8[5];
    uint8_t len;
    uint8_t wide;
    uint8_t cont;
    uint32_t fg;
    uint32_t bg;
};

struct out_buf {
    char *data;
    size_t len;
    size_t cap;
};

static struct {
    int forbid_auto;
    int active;            /* TTY_MODE_* */
    int in_fd;
    int out_fd;
    int custom_fds;        /* 1 when tty_backend_use_fds() is active */
    int custom_cols, custom_rows, custom_xpx, custom_ypx, custom_mode;

    struct termios orig_tio;
    int have_orig_tio;
    int atexit_done;

    /* Negotiated capabilities */
    int have_kitty_gfx;
    int have_sixel;
    int have_kitty_kbd;
    int truecolor;
    int mouse_pixel;

    /* Terminal geometry */
    int term_cols, term_rows;
    int px_w, px_h;
    int cell_w_px, cell_h_px;
    int canvas_w, canvas_h;
    double scale;

    /* Resize callbacks registered by screen.c */
    int (*on_pixel_resize)(int w, int h, double scale);
    int (*on_cell_resize)(int cols, int rows);

    /* Kitty strip ping-pong state */
    uint8_t row_gen[MAX_STRIP_ROWS];
    uint8_t row_has[MAX_STRIP_ROWS];
    uint8_t full_gen;
    uint8_t full_has;

    /* Cell fallback double-buffer */
    struct tty_cell *cur_cells;
    struct tty_cell *prev_cells;
    int cell_cols, cell_rows;
    int cell_full_repaint;

    /* Reusable scratch buffers */
    struct out_buf out;
    uint8_t *rgb_buf;
    size_t rgb_cap;
    uint8_t *z_buf;
    size_t z_cap;
    uint8_t *idx_buf;
    size_t idx_cap;

    /* Input buffer & decoded key queue */
    uint8_t in_buf[IN_BUF_CAP];
    size_t in_len;
    int64_t esc_since_ms;
    struct tty_key_ev kq[KEY_QUEUE_CAP];
    int kq_head, kq_len;
    int last_mods;
    char last_text[64];
    int shown;
    int eof;
    int mouse_ev;          /* 1 if SGR mouse decoded an event during parse_input */
} T = { .in_fd = -1, .out_fd = -1 };

/* Async-signal-safe teardown state */
static volatile sig_atomic_t sig_active;
static volatile sig_atomic_t sig_in_fd = -1;
static volatile sig_atomic_t sig_out_fd = -1;
static volatile sig_atomic_t sig_kitty_gfx;
static volatile sig_atomic_t sig_kitty_kbd;
static volatile sig_atomic_t sig_alt_screen;
static volatile sig_atomic_t sig_have_tio;
static struct termios sig_orig_tio;
static int winch_pipe[2] = { -1, -1 };
static pid_t sig_owner_pid; /* the process that installed the handlers; children must not touch the terminal */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_all_fd(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            if (poll(&pf, 1, 200) <= 0) break;
        } else {
            break;
        }
    }
}

static void on_winch(int sig)
{
    (void)sig;
    int saved = errno;
    if (winch_pipe[1] >= 0) {
        char b = 'W';
        ssize_t r = write(winch_pipe[1], &b, 1);
        (void)r;
    }
    errno = saved;
}

static void on_fatal_signal(int sig)
{
    if (sig_active && getpid() == sig_owner_pid) {
        sig_active = 0;
        int ofd = (int)sig_out_fd;
        if (ofd >= 0 && sig_alt_screen) {
            if (sig_kitty_gfx) {
                static const char del_gfx[] = "\x1b_Ga=d,d=A,q=2\x1b\\";
                ssize_t r = write(ofd, del_gfx, sizeof del_gfx - 1);
                (void)r;
            }
            if (sig_kitty_kbd) {
                static const char pop_kbd[] = "\x1b[<u";
                ssize_t r = write(ofd, pop_kbd, sizeof pop_kbd - 1);
                (void)r;
            }
            static const char leave[] =
                "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1016l"
                "\x1b[0m\x1b[?25h\x1b[?1049l";
            ssize_t r = write(ofd, leave, sizeof leave - 1);
            (void)r;
        }
        if (sig_have_tio && sig_in_fd >= 0) {
            tcsetattr((int)sig_in_fd, TCSANOW, &sig_orig_tio);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int out_reserve(size_t extra)
{
    if (T.out.len + extra <= T.out.cap) return 0;
    size_t ncap = T.out.cap ? T.out.cap * 2 : 16384;
    while (ncap < T.out.len + extra) {
        if (ncap > (size_t)64 * 1024 * 1024) return -1;
        ncap *= 2;
    }
    char *p = realloc(T.out.data, ncap);
    if (!p) return -1;
    T.out.data = p;
    T.out.cap = ncap;
    return 0;
}

static void out_append(const void *data, size_t len)
{
    if (len == 0 || out_reserve(len) < 0) return;
    memcpy(T.out.data + T.out.len, data, len);
    T.out.len += len;
}

static void out_puts(const char *s)
{
    out_append(s, strlen(s));
}

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1;
        out_append(tmp, len);
    }
}

static void out_flush(void)
{
    if (T.out.len > 0 && T.out_fd >= 0) {
        write_all_fd(T.out_fd, T.out.data, T.out.len);
        T.out.len = 0;
    }
}

static void out_base64_chunk(const uint8_t *src, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t enc_len = ((len + 2) / 3) * 4;
    if (out_reserve(enc_len) < 0) return;
    char *dst = T.out.data + T.out.len;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        *dst++ = tbl[(v >> 18) & 63];
        *dst++ = tbl[(v >> 12) & 63];
        *dst++ = tbl[(v >> 6) & 63];
        *dst++ = tbl[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) v |= (uint32_t)src[i + 1] << 8;
        *dst++ = tbl[(v >> 18) & 63];
        *dst++ = tbl[(v >> 12) & 63];
        *dst++ = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        *dst++ = '=';
    }
    T.out.len += enc_len;
}

/* --- Backend selection --- */

int tty_backend_explicit(void)
{
    const char *mode = getenv("LOCACHY_BACKEND");
    return mode && strcmp(mode, "tty") == 0;
}

void tty_backend_forbid_auto(void)
{
    T.forbid_auto = 1;
}

int tty_backend_wanted(void)
{
    const char *mode = getenv("LOCACHY_BACKEND");
    if (mode && *mode) {
        return strcmp(mode, "tty") == 0;
    }
    if (T.forbid_auto) return 0;
    if (control_fd && control_fd() >= 0) return 0;
    if (T.custom_fds) return 1;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    const char *ssh_tty = getenv("SSH_TTY");
    const char *ssh_conn = getenv("SSH_CONNECTION");
    if ((ssh_tty && *ssh_tty) || (ssh_conn && *ssh_conn)) return 1;
    char name[128] = "";
    if (ttyname_r(STDIN_FILENO, name, sizeof name) == 0 && strncmp(name, "/dev/pts/", 9) == 0) {
        return 1;
    }
    return 0;
}

int tty_backend_active(void) { return T.active; }
int tty_backend_pixel_mode(void)
{
    return T.active == TTY_MODE_PIXEL_KITTY || T.active == TTY_MODE_PIXEL_SIXEL;
}
int tty_backend_cell_mode(void) { return T.active == TTY_MODE_CELL; }

void tty_backend_use_fds(int in_fd, int out_fd, int cols, int rows,
                         int xpixel, int ypixel, int force_mode)
{
    T.custom_fds = 1;
    T.in_fd = in_fd;
    T.out_fd = out_fd;
    T.custom_cols = cols > 0 ? cols : 80;
    T.custom_rows = rows > 0 ? rows : 24;
    T.custom_xpx = xpixel > 0 ? xpixel : 0;
    T.custom_ypx = ypixel > 0 ? ypixel : 0;
    T.custom_mode = force_mode;
}

void tty_backend_reset_fds(void)
{
    T.custom_fds = 0;
    T.custom_mode = 0;
    T.in_fd = -1;
    T.out_fd = -1;
}

/* --- UTF-8 & CJK width (matches screen.c:207 exactly) --- */

static int tty_is_wide_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE4F) ||
           (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

static uint32_t utf8_decode_one(const uint8_t **pp, const uint8_t *end, size_t *nbytes)
{
    const uint8_t *p = *pp;
    uint8_t b0 = *p;
    if (b0 < 0x80) {
        *pp = p + 1;
        *nbytes = 1;
        return b0;
    }
    size_t need = 0;
    uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) { need = 2; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { need = 3; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { need = 4; cp = b0 & 0x07u; }
    else { *pp = p + 1; *nbytes = 1; return 0xFFFDu; }
    if ((size_t)(end - p) < need) {
        *pp = p + 1;
        *nbytes = 1;
        return 0xFFFDu;
    }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp = p + 1;
            *nbytes = 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    if ((need == 2 && cp < 0x80) ||
        (need == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
        (need == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
        *pp = p + 1;
        *nbytes = 1;
        return 0xFFFDu;
    }
    *pp = p + need;
    *nbytes = need;
    return cp;
}

static size_t utf8_encode_one(uint32_t cp, char out[5])
{
    if (cp < 0x80) {
        out[0] = (char)cp; out[1] = '\0'; return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        out[2] = '\0'; return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        out[3] = '\0'; return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        out[4] = '\0'; return 4;
    }
    out[0] = '?'; out[1] = '\0'; return 1;
}

/* --- Terminal mode & geometry --- */

static int detect_truecolor(void)
{
    const char *ct = getenv("COLORTERM");
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return 1;
    const char *term = getenv("TERM");
    if (term && (strstr(term, "direct") || strstr(term, "kitty") || strstr(term, "ghostty") ||
                 strstr(term, "wezterm") || strstr(term, "foot") || strstr(term, "alacritty"))) {
        return 1;
    }
    return 0;
}

static void query_winsize(void)
{
    if (T.custom_fds) {
        T.term_cols = T.custom_cols;
        T.term_rows = T.custom_rows;
        T.px_w = T.custom_xpx;
        T.px_h = T.custom_ypx;
        return;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    if (ioctl(T.out_fd, TIOCGWINSZ, &ws) < 0) {
        ioctl(T.in_fd, TIOCGWINSZ, &ws);
    }
    T.term_cols = ws.ws_col > 0 ? ws.ws_col : 80;
    T.term_rows = ws.ws_row > 0 ? ws.ws_row : 24;
    if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
        T.px_w = ws.ws_xpixel;
        T.px_h = ws.ws_ypixel;
    }
}

static int parse_env_mode(void)
{
    if (T.custom_fds && T.custom_mode != 0) return T.custom_mode;
    const char *m = getenv("LOCACHY_TTY_MODE");
    if (!m || !*m || strcmp(m, "auto") == 0) return TTY_MODE_NONE;
    if (strcmp(m, "kitty") == 0) return TTY_MODE_PIXEL_KITTY;
    if (strcmp(m, "sixel") == 0) return TTY_MODE_PIXEL_SIXEL;
    if (strcmp(m, "cell") == 0) return TTY_MODE_CELL;
    return TTY_MODE_NONE;
}

static void enter_raw_termios(void)
{
    if (T.custom_fds) return;
    if (!T.have_orig_tio && tcgetattr(T.in_fd, &T.orig_tio) == 0) {
        T.have_orig_tio = 1;
        sig_orig_tio = T.orig_tio;
        sig_have_tio = 1;
    }
    if (!T.have_orig_tio) return;
    struct termios raw = T.orig_tio;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(T.in_fd, TCSANOW, &raw);
}

static void restore_cooked_termios(void)
{
    if (!T.custom_fds && T.have_orig_tio && T.in_fd >= 0) {
        tcsetattr(T.in_fd, TCSANOW, &T.orig_tio);
    }
}

/* Parse a DA1 reply "\x1b[?....c" for Sixel attribute ";4;" / ";4c" / "?4;". */
static int da1_has_sixel(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && !isdigit((unsigned char)s[i])) i++;
        if (i >= len) break;
        int val = 0;
        while (i < len && isdigit((unsigned char)s[i])) {
            val = val * 10 + (s[i++] - '0');
        }
        if (val == 4) return 1;
    }
    return 0;
}

/* Single round-trip in-band capability probe (terminated by DA1 "\x1b[c").
 * Works across SSH and sudo without relying on TERM_PROGRAM. */
static void probe_capabilities(int want_mode)
{
    T.have_kitty_gfx = 0;
    T.have_sixel = 0;
    T.have_kitty_kbd = 0;

    int probe_gfx = (want_mode != TTY_MODE_CELL);
    out_puts("");
    if (probe_gfx) {
        /* Kitty graphics support probe (i=31) */
        out_puts("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
        if (T.px_w <= 0 || T.px_h <= 0) {
            /* Query terminal pixel area: reply is CSI 4 ; height ; width t */
            out_puts("\x1b[14t");
        }
    }
    /* Kitty keyboard protocol query + DA1 sentinel */
    out_puts("\x1b[?u\x1b[c");
    out_flush();

    uint8_t buf[1024];
    size_t len = 0;
    int got_da1 = 0;
    int64_t deadline = now_ms() + PROBE_TIMEOUT_MS;

    while (!got_da1 && len < sizeof buf - 1) {
        int64_t left = deadline - now_ms();
        if (left <= 0) break;
        struct pollfd pf = { .fd = T.in_fd, .events = POLLIN };
        int r = poll(&pf, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || !(pf.revents & POLLIN)) break;
        ssize_t n = read(T.in_fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = '\0';

        /* Check if DA1 reply ESC [ ? ... c has arrived. */
        for (size_t i = 0; i + 3 < len; i++) {
            if (buf[i] == 0x1b && buf[i + 1] == '[' && buf[i + 2] == '?') {
                for (size_t j = i + 3; j < len; j++) {
                    if (buf[j] >= 0x40 && buf[j] <= 0x7e) {
                        if (buf[j] == 'c') got_da1 = 1;
                        break;
                    }
                }
            }
        }
    }

    /* Consume recognized probe replies; keep any user typeahead bytes in T.in_buf. */
    size_t i = 0;
    while (i < len) {
        if (buf[i] == 0x1b && i + 1 < len && buf[i + 1] == '_') {
            /* APC ... ST (ESC \) */
            size_t end = i + 2;
            while (end + 1 < len && !(buf[end] == 0x1b && buf[end + 1] == '\\')) end++;
            if (end + 1 < len) {
                size_t alen = end - (i + 2);
                const char *apc = (const char *)(buf + i + 2);
                if (alen >= 6 && strncmp(apc, "Gi=31;", 6) == 0 && strstr(apc, "OK") != NULL) {
                    T.have_kitty_gfx = 1;
                }
                i = end + 2;
                continue;
            }
        }
        if (buf[i] == 0x1b && i + 1 < len && buf[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && !(buf[j] >= 0x40 && buf[j] <= 0x7e)) j++;
            if (j < len) {
                char fin = (char)buf[j];
                if (buf[i + 2] == '?' && fin == 'u') {
                    T.have_kitty_kbd = 1;
                    i = j + 1;
                    continue;
                }
                if (buf[i + 2] == '?' && fin == 'c') {
                    if (da1_has_sixel((const char *)(buf + i + 3), j - (i + 3))) {
                        T.have_sixel = 1;
                    }
                    i = j + 1;
                    continue;
                }
                if (fin == 't') {
                    int a = 0, b = 0, c = 0;
                    if (sscanf((const char *)(buf + i + 2), "%d;%d;%dt", &a, &b, &c) == 3 &&
                        a == 4 && b > 0 && c > 0) {
                        T.px_h = b;
                        T.px_w = c;
                    }
                    i = j + 1;
                    continue;
                }
            }
        }
        if (T.in_len < IN_BUF_CAP) {
            T.in_buf[T.in_len++] = buf[i];
        }
        i++;
    }
}

static void reset_kitty_strips(void)
{
    memset(T.row_gen, 0, sizeof T.row_gen);
    memset(T.row_has, 0, sizeof T.row_has);
    T.full_gen = 0;
    T.full_has = 0;
}

static int apply_pixel_geometry(void)
{
    int w = T.px_w, h = T.px_h;
    if (w < 320 || h < 200) return -1;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    int rows = T.term_rows > 0 ? T.term_rows : 24;
    int cols = T.term_cols > 0 ? T.term_cols : 80;
    int ch = h / rows;
    int cw = w / cols;
    if (ch >= 8 && cw >= 4 && rows <= MAX_STRIP_ROWS) {
        T.cell_h_px = ch;
        T.cell_w_px = cw;
        h = rows * ch; /* exact alignment with terminal rows */
    } else {
        T.cell_h_px = 0;
        T.cell_w_px = 0;
    }
    double raw_scale = T.cell_h_px > 0 ? (double)T.cell_h_px / (double)TEXT_H : 1.0;
    double s = floor(raw_scale * 4.0 + 0.5) / 4.0;
    if (s < 1.0) s = 1.0;
    if (s > 3.0) s = 3.0;
    T.canvas_w = w;
    T.canvas_h = h;
    T.scale = s;
    reset_kitty_strips();
    if (T.active == TTY_MODE_PIXEL_KITTY) {
        out_puts("\x1b_Ga=d,d=A,q=2\x1b\\\x1b[2J\x1b[H");
        out_flush();
    } else if (T.active == TTY_MODE_PIXEL_SIXEL) {
        out_puts("\x1b[2J\x1b[H");
        out_flush();
    }
    if (T.on_pixel_resize && T.on_pixel_resize(w, h, s) < 0) return -1;
    T.shown = 1;
    return 0;
}

static int apply_cell_geometry(void)
{
    int cols = T.term_cols > 1 ? T.term_cols : 80;
    int rows = T.term_rows > 1 ? T.term_rows : 24;
    if (cols > 512) cols = 512;
    if (rows > 256) rows = 256;
    size_t n = (size_t)cols * (size_t)rows;
    struct tty_cell *ncur = calloc(n, sizeof *ncur);
    struct tty_cell *nprev = calloc(n, sizeof *nprev);
    if (!ncur || !nprev) {
        free(ncur);
        free(nprev);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        ncur[i].utf8[0] = ' ';
        ncur[i].len = 1;
        ncur[i].fg = 0xD0D0D0u;
        ncur[i].bg = 0x101820u;
    }
    free(T.cur_cells);
    free(T.prev_cells);
    T.cur_cells = ncur;
    T.prev_cells = nprev;
    T.cell_cols = cols;
    T.cell_rows = rows;
    T.cell_full_repaint = 1;
    if (T.on_cell_resize && T.on_cell_resize(cols, rows) < 0) return -1;
    T.shown = 1;
    return 0;
}

static void emit_enter_terminal(void)
{
    /* Alternate screen, hide cursor, reset SGR, clear */
    out_puts("\x1b[?1049h\x1b[?25l\x1b[0m\x1b[2J\x1b[H");
    /* Mouse: button click + drag + wheel; SGR pixels (1016) in pixel mode, SGR cells (1006) in cell mode */
    if (T.active == TTY_MODE_PIXEL_KITTY || T.active == TTY_MODE_PIXEL_SIXEL) {
        T.mouse_pixel = 1;
        out_puts("\x1b[?1000h\x1b[?1002h\x1b[?1016h");
    } else {
        T.mouse_pixel = 0;
        out_puts("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    }
    if (T.have_kitty_kbd) {
        /* Progressive enhancement flag 1: disambiguate escape codes (Esc, Ctrl/Alt/Shift combos) */
        out_puts("\x1b[>1u");
    }
    out_flush();
}

static void emit_leave_terminal(void)
{
    if (T.active == TTY_MODE_PIXEL_KITTY) {
        out_puts("\x1b_Ga=d,d=A,q=2\x1b\\");
    }
    if (T.have_kitty_kbd) {
        out_puts("\x1b[<u");
    }
    out_puts("\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1016l"
             "\x1b[0m\x1b[?25h\x1b[?1049l");
    out_flush();
}

int tty_backend_open(int (*resize)(int width, int height, double scale),
                     int (*cell_resize)(int cols, int rows))
{
    if (T.active) return 0;
    if (!T.custom_fds) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            say("tty: stdin/stdout is not a terminal");
            return -1;
        }
        T.in_fd = STDIN_FILENO;
        T.out_fd = STDOUT_FILENO;
    }
    T.on_pixel_resize = resize;
    T.on_cell_resize = cell_resize;
    T.in_len = 0;
    T.kq_head = T.kq_len = 0;
    T.eof = 0;
    T.esc_since_ms = 0;
    T.mouse_ev = 0;
    T.truecolor = detect_truecolor();

    /* Teardown is armed before the terminal changes: the probe below waits for replies. */
    if (!T.custom_fds) {
        sig_owner_pid = getpid();
        sig_in_fd = T.in_fd;
        sig_out_fd = T.out_fd;
        sig_kitty_gfx = 0;
        sig_kitty_kbd = 0;
        sig_alt_screen = 0;
        sig_active = 1;

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fatal_signal;
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        if (!T.atexit_done) {
            atexit(tty_backend_close);
            T.atexit_done = 1;
        }
    }

    enter_raw_termios();
    query_winsize();

    int want = parse_env_mode();
    if (T.custom_fds && T.custom_mode != 0) {
        T.have_kitty_gfx = (T.custom_mode == TTY_MODE_PIXEL_KITTY);
        T.have_sixel = (T.custom_mode == TTY_MODE_PIXEL_SIXEL);
        T.have_kitty_kbd = 0;
    } else {
        probe_capabilities(want);
    }

    int can_pixel = (T.px_w >= 320 && T.px_h >= 200);
    if (want == TTY_MODE_PIXEL_KITTY) {
        if (can_pixel) {
            T.active = TTY_MODE_PIXEL_KITTY;
        } else {
            T.active = TTY_MODE_CELL;
            say("tty: LOCACHY_TTY_MODE=kitty requested but terminal reported no pixel size; falling back to cell mode");
        }
    } else if (want == TTY_MODE_PIXEL_SIXEL) {
        if (can_pixel) {
            T.active = TTY_MODE_PIXEL_SIXEL;
        } else {
            T.active = TTY_MODE_CELL;
            say("tty: LOCACHY_TTY_MODE=sixel requested but terminal reported no pixel size; falling back to cell mode");
        }
    } else if (want == TTY_MODE_CELL) {
        T.active = TTY_MODE_CELL;
    } else {
        if (T.have_kitty_gfx && can_pixel) T.active = TTY_MODE_PIXEL_KITTY;
        else if (T.have_sixel && can_pixel) T.active = TTY_MODE_PIXEL_SIXEL;
        else T.active = TTY_MODE_CELL;
    }

    if (!T.custom_fds) {
        if (winch_pipe[0] < 0 && pipe2(winch_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
            winch_pipe[0] = winch_pipe[1] = -1;
        }
        sig_kitty_gfx = (T.active == TTY_MODE_PIXEL_KITTY);
        sig_kitty_kbd = T.have_kitty_kbd;
        sig_alt_screen = 1;

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_winch;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, NULL);
    }

    emit_enter_terminal();

    int rc = tty_backend_pixel_mode() ? apply_pixel_geometry() : apply_cell_geometry();
    if (rc < 0) {
        if (tty_backend_pixel_mode()) {
            /* Pixel resize failed (e.g. missing font): fall back cleanly to cell mode */
            say("tty: pixel canvas setup failed; falling back to cell mode");
            T.active = TTY_MODE_CELL;
            sig_kitty_gfx = 0;
            T.mouse_pixel = 0;
            out_puts("\x1b[?1016l\x1b[?1006h");
            out_flush();
            rc = apply_cell_geometry();
        }
        if (rc < 0) {
            tty_backend_close();
            return -1;
        }
    }

    if (pointer_window_present) pointer_window_present(1);
    const char *mname = T.active == TTY_MODE_PIXEL_KITTY ? "kitty-fb" :
                        T.active == TTY_MODE_PIXEL_SIXEL ? "sixel-fb" : "ansi-cell";
    say("tty: mode=%s (%dx%d cells, %dx%d px, scale %.2f, kitty-kbd=%d, truecolor=%d)",
        mname, T.term_cols, T.term_rows, T.canvas_w, T.canvas_h, T.scale,
        T.have_kitty_kbd, T.truecolor);
    return 0;
}

void tty_backend_close(void)
{
    if (!T.active) return;
    /* A forked child that ends without exec (auth.c) must leave its parent's terminal alone. */
    if (sig_owner_pid && getpid() != sig_owner_pid) return;
    sig_active = 0;
    if (pointer_window_present) pointer_window_present(0);
    emit_leave_terminal();
    restore_cooked_termios();

    if (!T.custom_fds) {
        signal(SIGWINCH, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (winch_pipe[0] >= 0) { close(winch_pipe[0]); winch_pipe[0] = -1; }
        if (winch_pipe[1] >= 0) { close(winch_pipe[1]); winch_pipe[1] = -1; }
    }

    free(T.cur_cells); T.cur_cells = NULL;
    free(T.prev_cells); T.prev_cells = NULL;
    free(T.out.data); T.out.data = NULL; T.out.len = T.out.cap = 0;
    free(T.rgb_buf); T.rgb_buf = NULL; T.rgb_cap = 0;
    free(T.z_buf); T.z_buf = NULL; T.z_cap = 0;
    free(T.idx_buf); T.idx_buf = NULL; T.idx_cap = 0;
    T.active = TTY_MODE_NONE;
    T.have_orig_tio = 0;
    T.canvas_w = T.canvas_h = 0;
    T.px_w = T.px_h = 0;
    T.cell_w_px = T.cell_h_px = 0;
    T.scale = 1.0;
    sig_have_tio = 0;
    sig_alt_screen = 0;
    sig_owner_pid = 0;
}

/* --- Pixel Framebuffer Flush: Kitty Graphics Protocol --- */

static int ensure_rgb_buf(size_t need)
{
    if (need <= T.rgb_cap) return 0;
    uint8_t *p = realloc(T.rgb_buf, need);
    if (!p) return -1;
    T.rgb_buf = p;
    T.rgb_cap = need;
    return 0;
}

static void kitty_send_image(const uint32_t *pixels, int stride, int w, int h,
                             uint32_t img_id, uint32_t old_id)
{
    size_t rgb_len = (size_t)w * (size_t)h * 3;
    if (ensure_rgb_buf(rgb_len) < 0) return;
    uint8_t *dst = T.rgb_buf;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = pixels + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];
            *dst++ = (uint8_t)((px >> 16) & 0xFFu);
            *dst++ = (uint8_t)((px >> 8) & 0xFFu);
            *dst++ = (uint8_t)(px & 0xFFu);
        }
    }

    const uint8_t *payload = T.rgb_buf;
    size_t payload_len = rgb_len;
    int compressed = 0;

#ifdef CONFIG_ZLIB
    uLongf zbound = compressBound((uLong)rgb_len);
    if (zbound > T.z_cap) {
        uint8_t *p = realloc(T.z_buf, zbound);
        if (p) { T.z_buf = p; T.z_cap = zbound; }
    }
    if (T.z_buf && T.z_cap >= zbound) {
        uLongf zlen = (uLongf)T.z_cap;
        if (compress2(T.z_buf, &zlen, T.rgb_buf, (uLong)rgb_len, Z_BEST_SPEED) == Z_OK &&
            zlen < rgb_len) {
            payload = T.z_buf;
            payload_len = (size_t)zlen;
            compressed = 1;
        }
    }
#endif

    /* Transmit in <= 3072 raw-byte chunks (= 4096 base64 chars, Kitty spec max chunk). */
    const size_t chunk_raw = 3072;
    size_t off = 0;
    int first = 1;
    while (off < payload_len || first) {
        size_t take = payload_len - off;
        if (take > chunk_raw) take = chunk_raw;
        int more = (off + take < payload_len) ? 1 : 0;
        if (first) {
            out_printf("\x1b_Ga=T,f=24,t=d,i=%u,p=1,s=%d,v=%d,C=1,q=2%s,m=%d;",
                       img_id, w, h, compressed ? ",o=z" : "", more);
            first = 0;
        } else {
            out_printf("\x1b_Gm=%d;", more);
        }
        if (take > 0) out_base64_chunk(payload + off, take);
        out_puts("\x1b\\");
        off += take;
    }

    if (old_id != 0) {
        out_printf("\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", old_id);
    }
}

static int flush_kitty(const uint32_t *pixels, int w, int h, int x0, int y0, int x1, int y1)
{
    (void)x0; (void)x1;
    if (w <= 0 || h <= 0 || y0 >= y1) return 0;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;

    out_puts("\x1b[?2026h");
    if (T.cell_h_px > 0 && T.term_rows > 0 && T.term_rows <= MAX_STRIP_ROWS &&
        h == T.term_rows * T.cell_h_px) {
        int r0 = y0 / T.cell_h_px;
        int r1 = (y1 - 1) / T.cell_h_px;
        if (r0 < 0) r0 = 0;
        if (r1 >= T.term_rows) r1 = T.term_rows - 1;
        for (int r = r0; r <= r1; r++) {
            uint32_t next_id = 10u + (uint32_t)r + (T.row_gen[r] ? (uint32_t)MAX_STRIP_ROWS : 0u);
            uint32_t old_id = T.row_has[r]
                ? (10u + (uint32_t)r + (T.row_gen[r] ? 0u : (uint32_t)MAX_STRIP_ROWS))
                : 0u;
            out_printf("\x1b[%d;1H", r + 1);
            kitty_send_image(pixels + (size_t)r * (size_t)T.cell_h_px * (size_t)w,
                             w, w, T.cell_h_px, next_id, old_id);
            T.row_gen[r] ^= 1u;
            T.row_has[r] = 1u;
        }
    } else {
        uint32_t next_id = T.full_gen ? 2u : 1u;
        uint32_t old_id = T.full_has ? (T.full_gen ? 1u : 2u) : 0u;
        out_puts("\x1b[1;1H");
        kitty_send_image(pixels, w, w, h, next_id, old_id);
        T.full_gen ^= 1u;
        T.full_has = 1u;
    }
    out_puts("\x1b[?2026l");
    out_flush();
    return 0;
}

/* --- Pixel Framebuffer Flush: Sixel Graphics --- */

static uint8_t rgb_to_sixel_fallback(uint32_t rgb)
{
    int r = (int)((rgb >> 16) & 0xFFu);
    int g = (int)((rgb >> 8) & 0xFFu);
    int b = (int)(rgb & 0xFFu);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (hi - lo < 12) {
        int grey = (r * 77 + g * 150 + b * 29) >> 8;
        return (uint8_t)(216 + (grey * 31 + 127) / 255);
    }
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    return (uint8_t)(ri * 36 + gi * 6 + bi);
}

static uint32_t sixel_fallback_rgb(uint8_t idx)
{
    if (idx >= 216) {
        int g = ((idx - 216) * 255 + 15) / 31;
        return ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
    }
    int ri = idx / 36, gi = (idx / 6) % 6, bi = idx % 6;
    uint32_t r = (uint32_t)(ri * 255 / 5);
    uint32_t g = (uint32_t)(gi * 255 / 5);
    uint32_t b = (uint32_t)(bi * 255 / 5);
    return (r << 16) | (g << 8) | b;
}

static void sixel_emit_run(char ch, int count)
{
    if (count <= 0) return;
    if (count >= 4) {
        out_printf("!%d%c", count, ch);
    } else {
        char tmp[4];
        for (int i = 0; i < count; i++) tmp[i] = ch;
        out_append(tmp, (size_t)count);
    }
}

static int flush_sixel(const uint32_t *pixels, int w, int h, int x0, int y0, int x1, int y1)
{
    (void)x0; (void)x1;
    if (w <= 0 || h <= 0 || y0 >= y1) return 0;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;

    /* Align dirty band vertically to a cell-row multiple that is also a multiple of 6 px. */
    int start_row = 0, by0 = 0, by1 = h;
    if (T.cell_h_px > 0 && T.term_rows > 0 && h == T.term_rows * T.cell_h_px) {
        int step_rows = 1;
        if (T.cell_h_px % 6 == 0) step_rows = 1;
        else if (T.cell_h_px % 3 == 0) step_rows = 2;
        else if (T.cell_h_px % 2 == 0) step_rows = 3;
        else step_rows = 6;
        int r0 = (y0 / T.cell_h_px) / step_rows * step_rows;
        int r1 = (((y1 - 1) / T.cell_h_px) / step_rows + 1) * step_rows;
        if (r1 > T.term_rows) r1 = T.term_rows;
        start_row = r0;
        by0 = r0 * T.cell_h_px;
        by1 = r1 * T.cell_h_px;
    }

    int bh = by1 - by0;
    size_t npix = (size_t)w * (size_t)bh;
    if (npix > T.idx_cap) {
        uint8_t *p = realloc(T.idx_buf, npix);
        if (!p) return -1;
        T.idx_buf = p;
        T.idx_cap = npix;
    }

    /* Try lossless palette first (up to 256 unique colors — typical TUIs use < 64). */
    uint32_t hkeys[512];
    uint8_t hvals[512];
    uint32_t pal[256];
    memset(hkeys, 0, sizeof hkeys);
    int ncolors = 0, overflow = 0;

    for (int y = 0; y < bh && !overflow; y++) {
        const uint32_t *row = pixels + (size_t)(by0 + y) * (size_t)w;
        uint8_t *irow = T.idx_buf + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t rgb = row[x] & 0xFFFFFFu;
            uint32_t key = rgb | 0x01000000u;
            uint32_t slot = ((rgb * 2654435761u) >> 23) & 511u;
            while (hkeys[slot] != 0 && hkeys[slot] != key) {
                slot = (slot + 1u) & 511u;
            }
            if (hkeys[slot] == key) {
                irow[x] = hvals[slot];
            } else if (ncolors < 256) {
                hkeys[slot] = key;
                hvals[slot] = (uint8_t)ncolors;
                pal[ncolors] = rgb;
                irow[x] = (uint8_t)ncolors;
                ncolors++;
            } else {
                overflow = 1;
                break;
            }
        }
    }

    uint8_t used_any[256];
    memset(used_any, 0, sizeof used_any);
    if (overflow) {
        ncolors = 248;
        for (int i = 0; i < ncolors; i++) pal[i] = sixel_fallback_rgb((uint8_t)i);
        for (int y = 0; y < bh; y++) {
            const uint32_t *row = pixels + (size_t)(by0 + y) * (size_t)w;
            uint8_t *irow = T.idx_buf + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                uint8_t idx = rgb_to_sixel_fallback(row[x] & 0xFFFFFFu);
                irow[x] = idx;
                used_any[idx] = 1;
            }
        }
    } else {
        for (int i = 0; i < ncolors; i++) used_any[i] = 1;
    }

    out_puts("\x1b[?2026h");
    out_printf("\x1b[%d;1H\x1bP0;1;0q\"1;1;%d;%d", start_row + 1, w, bh);
    for (int i = 0; i < ncolors; i++) {
        if (!used_any[i]) continue;
        int r = (int)((((pal[i] >> 16) & 0xFFu) * 100u + 127u) / 255u);
        int g = (int)((((pal[i] >> 8) & 0xFFu) * 100u + 127u) / 255u);
        int b = (int)(((pal[i] & 0xFFu) * 100u + 127u) / 255u);
        out_printf("#%d;2;%d;%d;%d", i, r, g, b);
    }

    for (int band = 0; band < bh; band += 6) {
        int bmax = bh - band < 6 ? bh - band : 6;
        uint8_t band_used[256];
        memset(band_used, 0, sizeof band_used);
        for (int dy = 0; dy < bmax; dy++) {
            const uint8_t *irow = T.idx_buf + (size_t)(band + dy) * (size_t)w;
            for (int x = 0; x < w; x++) band_used[irow[x]] = 1;
        }
        int first_col = 1;
        for (int c = 0; c < ncolors; c++) {
            if (!band_used[c]) continue;
            if (!first_col) out_append("$", 1);
            first_col = 0;
            out_printf("#%d", c);
            char run_ch = 0;
            int run_len = 0;
            for (int x = 0; x < w; x++) {
                uint8_t bits = 0;
                for (int dy = 0; dy < bmax; dy++) {
                    if (T.idx_buf[(size_t)(band + dy) * (size_t)w + (size_t)x] == (uint8_t)c) {
                        bits |= (uint8_t)(1u << dy);
                    }
                }
                char ch = (char)('?' + bits);
                if (ch == run_ch) {
                    run_len++;
                } else {
                    sixel_emit_run(run_ch, run_len);
                    run_ch = ch;
                    run_len = 1;
                }
            }
            /* Trim trailing empty sixels ('?') at end of band row for speed. */
            if (run_ch != '?') sixel_emit_run(run_ch, run_len);
        }
        if (band + 6 < bh) out_append("-", 1);
    }
    out_puts("\x1b\\\x1b[?2026l");
    out_flush();
    return 0;
}

int tty_backend_flush_pixels(const uint32_t *pixels, int w, int h, int x0, int y0, int x1, int y1)
{
    if (T.active == TTY_MODE_PIXEL_KITTY) return flush_kitty(pixels, w, h, x0, y0, x1, y1);
    if (T.active == TTY_MODE_PIXEL_SIXEL) return flush_sixel(pixels, w, h, x0, y0, x1, y1);
    return -1;
}

/* --- ANSI Cell Fallback --- */

static uint8_t rgb_to_xterm256(uint32_t rgb)
{
    int r = (int)((rgb >> 16) & 0xFFu);
    int g = (int)((rgb >> 8) & 0xFFu);
    int b = (int)(rgb & 0xFFu);

    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int ri = 0, gi = 0, bi = 0;
    for (int i = 1; i < 6; i++) {
        if (abs(r - steps[i]) < abs(r - steps[ri])) ri = i;
        if (abs(g - steps[i]) < abs(g - steps[gi])) gi = i;
        if (abs(b - steps[i]) < abs(b - steps[bi])) bi = i;
    }
    int dr = r - steps[ri], dg = g - steps[gi], db = b - steps[bi];
    int cube_dist = dr * dr + dg * dg + db * db;
    uint8_t best = (uint8_t)(16 + 36 * ri + 6 * gi + bi);

    /* Compare against 24 greyscale entries (232..255: 8 + 10*k). */
    int grey = (r * 77 + g * 150 + b * 29) >> 8;
    int ki = (grey - 8 + 5) / 10;
    if (ki < 0) ki = 0;
    if (ki > 23) ki = 23;
    int gv = 8 + 10 * ki;
    int gr = r - gv, gg = g - gv, gb = b - gv;
    int grey_dist = gr * gr + gg * gg + gb * gb;
    if (grey_dist < cube_dist) best = (uint8_t)(232 + ki);
    return best;
}

static void cell_clear_orphan(int col, int row)
{
    if (!T.cur_cells || row < 0 || row >= T.cell_rows || col < 0 || col >= T.cell_cols) return;
    struct tty_cell *c = &T.cur_cells[(size_t)row * (size_t)T.cell_cols + (size_t)col];
    if (c->cont && col > 0) {
        struct tty_cell *left = c - 1;
        left->utf8[0] = ' '; left->utf8[1] = '\0';
        left->len = 1; left->wide = 0; left->cont = 0;
    } else if (c->wide && col + 1 < T.cell_cols) {
        struct tty_cell *right = c + 1;
        right->utf8[0] = ' '; right->utf8[1] = '\0';
        right->len = 1; right->wide = 0; right->cont = 0;
    }
}

int tty_cell_cols(void) { return T.cell_cols; }
int tty_cell_rows(void) { return T.cell_rows; }

void tty_cell_fill(int col, int row, int ncols, int nrows, uint32_t rgb)
{
    if (!T.cur_cells || ncols <= 0 || nrows <= 0) return;
    if (col < 0) { ncols += col; col = 0; }
    if (row < 0) { nrows += row; row = 0; }
    if (col >= T.cell_cols || row >= T.cell_rows || ncols <= 0 || nrows <= 0) return;
    if (col + ncols > T.cell_cols) ncols = T.cell_cols - col;
    if (row + nrows > T.cell_rows) nrows = T.cell_rows - row;
    uint32_t c = rgb & 0xFFFFFFu;
    for (int r = row; r < row + nrows; r++) {
        cell_clear_orphan(col, r);
        cell_clear_orphan(col + ncols - 1, r);
        struct tty_cell *line = T.cur_cells + (size_t)r * (size_t)T.cell_cols;
        for (int x = col; x < col + ncols; x++) {
            line[x].utf8[0] = ' ';
            line[x].utf8[1] = '\0';
            line[x].len = 1;
            line[x].wide = 0;
            line[x].cont = 0;
            line[x].fg = c;
            line[x].bg = c;
        }
    }
}

void tty_cell_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (!T.cur_cells || !s || row < 0 || row >= T.cell_rows) return;
    uint32_t fg_rgb = fg & 0xFFFFFFu;
    uint32_t bg_rgb = bg & 0xFFFFFFu;
    const uint8_t *p = (const uint8_t *)s;
    const uint8_t *end = p + strlen(s);
    struct tty_cell *line = T.cur_cells + (size_t)row * (size_t)T.cell_cols;

    while (p < end && col < T.cell_cols) {
        size_t nb = 0;
        const uint8_t *start = p;
        uint32_t cp = utf8_decode_one(&p, end, &nb);
        /* C0, DEL and C1 controls are never printed; a byte that is not UTF-8 shows as U+FFFD. */
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;
        const uint8_t *src = start;
        size_t n = nb;
        if (cp == 0xFFFD && n == 1) {
            src = (const uint8_t *)"\xEF\xBF\xBD";
            n = 3;
        }
        int wide = tty_is_wide_cjk(cp);
        if (col < 0) {
            col += wide ? 2 : 1;
            continue;
        }
        if (wide && col + 1 >= T.cell_cols) {
            cell_clear_orphan(col, row);
            line[col].utf8[0] = ' ';
            line[col].utf8[1] = '\0';
            line[col].len = 1;
            line[col].wide = 0;
            line[col].cont = 0;
            line[col].fg = fg_rgb;
            line[col].bg = bg_rgb;
            break;
        }
        cell_clear_orphan(col, row);
        if (wide) cell_clear_orphan(col + 1, row);
        memcpy(line[col].utf8, src, n);
        line[col].utf8[n] = '\0';
        line[col].len = (uint8_t)n;
        line[col].wide = (uint8_t)wide;
        line[col].cont = 0;
        line[col].fg = fg_rgb;
        line[col].bg = bg_rgb;
        if (wide) {
            line[col + 1].utf8[0] = '\0';
            line[col + 1].len = 0;
            line[col + 1].wide = 0;
            line[col + 1].cont = 1;
            line[col + 1].fg = fg_rgb;
            line[col + 1].bg = bg_rgb;
            col += 2;
        } else {
            col += 1;
        }
    }
}

void tty_cell_bar(const char *title, const char *ime)
{
    if (!T.cur_cells || T.cell_rows < 1) return;
    uint32_t bg = theme_rgb(THEME_PANEL, PANEL_BG & 0xFFFFFFu);
    uint32_t fg = theme_rgb(THEME_FG, 0xD0D0D0u);
    uint32_t acc = theme_rgb(THEME_ACCENT_TEXT, 0x80C0FFu);
    tty_cell_fill(0, 0, T.cell_cols, 1, bg);
    tty_cell_text(1, 0, "LOCachy", acc, bg);
    if (title && *title) {
        char buf[128];
        snprintf(buf, sizeof buf, " | %s", title);
        tty_cell_text(8, 0, buf, fg, bg);
    }
    char right[64] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) {
        if (ime && *ime) {
            snprintf(right, sizeof right, "[%s] %02d:%02d", ime, tm.tm_hour, tm.tm_min);
        } else {
            snprintf(right, sizeof right, "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
    }
    int rlen = (int)strlen(right);
    if (rlen > 0 && T.cell_cols - rlen - 1 > 16) {
        tty_cell_text(T.cell_cols - rlen - 1, 0, right, fg, bg);
    }
}

static int cell_equal(const struct tty_cell *a, const struct tty_cell *b)
{
    return a->len == b->len && a->wide == b->wide && a->cont == b->cont &&
           a->fg == b->fg && a->bg == b->bg &&
           memcmp(a->utf8, b->utf8, a->len) == 0;
}

void tty_cell_flush(void)
{
    if (!T.cur_cells || !T.prev_cells) return;
    int cols = T.cell_cols, rows = T.cell_rows;
    int full = T.cell_full_repaint;
    uint32_t cur_fg = 0xFFFFFFFFu, cur_bg = 0xFFFFFFFFu;
    int cur_r = -1, cur_c = -1;
    int any = 0;

    for (int r = 0; r < rows; r++) {
        const struct tty_cell *crow = T.cur_cells + (size_t)r * (size_t)cols;
        struct tty_cell *prow = T.prev_cells + (size_t)r * (size_t)cols;
        for (int c = 0; c < cols; c++) {
            if (crow[c].cont) continue;
            int dirty = full || !cell_equal(&crow[c], &prow[c]);
            if (!dirty && crow[c].wide && c + 1 < cols && !cell_equal(&crow[c + 1], &prow[c + 1])) {
                dirty = 1;
            }
            if (!dirty) continue;
            if (!any) {
                out_puts("\x1b[?2026h");
                any = 1;
            }
            if (cur_r != r || cur_c != c) {
                out_printf("\x1b[%d;%dH", r + 1, c + 1);
                cur_r = r;
                cur_c = c;
            }
            if (crow[c].fg != cur_fg) {
                cur_fg = crow[c].fg;
                if (T.truecolor) {
                    out_printf("\x1b[38;2;%u;%u;%um",
                               (cur_fg >> 16) & 0xFFu, (cur_fg >> 8) & 0xFFu, cur_fg & 0xFFu);
                } else {
                    out_printf("\x1b[38;5;%um", (unsigned)rgb_to_xterm256(cur_fg));
                }
            }
            if (crow[c].bg != cur_bg) {
                cur_bg = crow[c].bg;
                if (T.truecolor) {
                    out_printf("\x1b[48;2;%u;%u;%um",
                               (cur_bg >> 16) & 0xFFu, (cur_bg >> 8) & 0xFFu, cur_bg & 0xFFu);
                } else {
                    out_printf("\x1b[48;5;%um", (unsigned)rgb_to_xterm256(cur_bg));
                }
            }
            out_append(crow[c].utf8, crow[c].len);
            cur_c += crow[c].wide ? 2 : 1;
            prow[c] = crow[c];
            if (crow[c].wide && c + 1 < cols) prow[c + 1] = crow[c + 1];
        }
    }
    if (any) {
        out_puts("\x1b[?2026l");
        out_flush();
    }
    T.cell_full_repaint = 0;
}

/* --- Keyboard & Mouse decoding --- */

static void kq_push(int key, int mods, const char *text)
{
    if (key == 0) return;
    if (T.kq_len == KEY_QUEUE_CAP) {
        T.kq_head = (T.kq_head + 1) % KEY_QUEUE_CAP;
        T.kq_len--;
    }
    struct tty_key_ev *ev = &T.kq[(T.kq_head + T.kq_len++) % KEY_QUEUE_CAP];
    ev->key = key;
    ev->mods = mods;
    if (text && text[0]) {
        snprintf(ev->text, sizeof ev->text, "%s", text);
    } else {
        ev->text[0] = '\0';
    }
}

static int decode_xterm_mods(int mod_param)
{
    if (mod_param <= 1) return 0;
    int m = mod_param - 1;
    int out = 0;
    if (m & 1) out |= KEYS_SHIFT;
    if (m & 2) out |= KEYS_ALT;
    if (m & 4) out |= KEYS_CTRL;
    if (m & 8) out |= KEYS_META;
    return out;
}

static void handle_sgr_mouse(const char *params, size_t plen, char fin)
{
    int b = 0, x = 0, y = 0;
    char tmp[64];
    if (plen >= sizeof tmp) return;
    memcpy(tmp, params, plen);
    tmp[plen] = '\0';
    if (sscanf(tmp, "%d;%d;%d", &b, &x, &y) != 3) return;
    int px = x > 0 ? x - 1 : 0;
    int py = y > 0 ? y - 1 : 0;
    if (pointer_window_motion) {
        pointer_window_motion(px, py);
        T.mouse_ev = 1;
    }

    int pressed = (fin == 'M');
    int motion = (b & 32) != 0;
    int wheel = (b & 64) != 0;
    int low = b & 3;

    if (wheel) {
        if (!pressed || !pointer_window_scroll) return;
        if (low == 0) pointer_window_scroll(SCROLL_WHEEL, -1.0, 0.0, 1);
        else if (low == 1) pointer_window_scroll(SCROLL_WHEEL, 1.0, 0.0, 1);
        else if (low == 2) pointer_window_scroll(SCROLL_WHEEL, 0.0, -1.0, 2);
        else if (low == 3) pointer_window_scroll(SCROLL_WHEEL, 0.0, 1.0, 2);
        T.mouse_ev = 1;
        return;
    }
    if (motion) return;

    int code = low == 0 ? BTN_LEFT : low == 1 ? BTN_MIDDLE : low == 2 ? BTN_RIGHT : 0;
    if (!code) return;
    if (pointer_window_button) {
        pointer_window_button(code, pressed);
        T.mouse_ev = 1;
    }
}

static void handle_kitty_u(const char *params, size_t plen)
{
    /* Format: codepoint[:shifted[:base]][;modifiers[:event_type]]u */
    char tmp[64];
    if (plen >= sizeof tmp) return;
    memcpy(tmp, params, plen);
    tmp[plen] = '\0';

    unsigned long cp = 0;
    int mods_param = 1, ev_type = 1;
    char *semi = strchr(tmp, ';');
    if (semi) {
        *semi++ = '\0';
        char *colon = strchr(semi, ':');
        if (colon) {
            *colon++ = '\0';
            ev_type = atoi(colon);
        }
        if (*semi) mods_param = atoi(semi);
    }
    if (ev_type == 3) return; /* ignore key release */

    char *shifted_s = strchr(tmp, ':');
    unsigned long shifted_cp = 0;
    if (shifted_s) {
        *shifted_s++ = '\0';
        char *base_s = strchr(shifted_s, ':');
        if (base_s) *base_s = '\0';
        if (*shifted_s) shifted_cp = strtoul(shifted_s, NULL, 10);
    }
    cp = strtoul(tmp, NULL, 10);
    int mods = decode_xterm_mods(mods_param);

    if (cp == 27) { kq_push(27, mods, NULL); return; }
    if (cp == 13 || cp == 10) { kq_push('\n', mods, NULL); return; }
    if (cp == 9) { kq_push('\t', mods, NULL); return; }
    if (cp == 127 || cp == 8) { kq_push('\b', mods, NULL); return; }
    if (cp == ' ' && (mods & KEYS_CTRL) && !(mods & KEYS_ALT)) {
        kq_push(K_CTRL_SPACE, mods, NULL);
        return;
    }
    if ((mods & KEYS_CTRL) && !(mods & KEYS_ALT)) {
        if (cp >= '0' && cp <= '9') {
            kq_push(K_CTRL_0 + (int)(cp - '0'), mods, NULL);
            return;
        }
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
            kq_push((int)(cp & 0x1Fu), mods, NULL);
            return;
        }
    }
    if ((mods & KEYS_SHIFT) && !(mods & (KEYS_CTRL | KEYS_ALT | KEYS_META))) {
        if (shifted_cp >= 0x20 && shifted_cp < 0x7F) cp = shifted_cp;
        else if (cp >= 'a' && cp <= 'z') cp -= 32;
    }
    if (cp >= 0x20 && cp < 0x7F) {
        kq_push((int)cp, mods, NULL);
        return;
    }
    if (cp >= 0x80 && cp <= 0x10FFFF && !(cp >= 0xE000 && cp <= 0xF8FF)) {
        char u8[5];
        utf8_encode_one((uint32_t)cp, u8);
        kq_push(K_TEXT, mods, u8);
    }
}

static void handle_csi(const uint8_t *seq, size_t len)
{
    /* seq[0..len-1] is everything after ESC [ including final byte seq[len-1]. */
    if (len == 0) return;
    char fin = (char)seq[len - 1];
    const char *params = (const char *)seq;
    size_t plen = len - 1;

    if (plen > 0 && params[0] == '<' && (fin == 'M' || fin == 'm')) {
        handle_sgr_mouse(params + 1, plen - 1, fin);
        return;
    }
    if (fin == 'u' && !(plen > 0 && params[0] == '?')) {
        handle_kitty_u(params, plen);
        return;
    }
    if (fin == 't') {
        int a = 0, b = 0, c = 0;
        char tmp[48];
        if (plen < sizeof tmp) {
            memcpy(tmp, params, plen); tmp[plen] = '\0';
            if (sscanf(tmp, "%d;%d;%d", &a, &b, &c) == 3 && a == 4 && b > 0 && c > 0) {
                if (b != T.px_h || c != T.px_w) {
                    T.px_h = b; T.px_w = c;
                    if (tty_backend_pixel_mode()) apply_pixel_geometry();
                }
            }
        }
        return;
    }

    /* Parse up to 3 numeric parameters p[0];p[1];p[2] (ignoring private prefix ?/>/=). */
    if (plen > 0 && (params[0] == '?' || params[0] == '>' || params[0] == '=')) return;
    int p[3] = { 0, 0, 0 };
    int np = 0;
    size_t i = 0;
    while (i < plen && np < 3) {
        int v = 0;
        while (i < plen && isdigit((unsigned char)params[i])) {
            v = v * 10 + (params[i++] - '0');
        }
        p[np++] = v;
        if (i < plen && params[i] == ':') {
            while (i < plen && params[i] != ';') i++;
        }
        if (i < plen && params[i] == ';') i++;
    }

    int mods = (np >= 2) ? decode_xterm_mods(p[1]) : 0;

    /* xterm modifyOtherKeys: CSI 27 ; mod ; keycode ~ */
    if (fin == '~' && np == 3 && p[0] == 27) {
        int code = p[2];
        if (code == 27) kq_push(27, mods, NULL);
        else if (code == 13 || code == 10) kq_push('\n', mods, NULL);
        else if (code == 9) kq_push('\t', mods, NULL);
        else if (code == 127 || code == 8) kq_push('\b', mods, NULL);
        else if ((mods & KEYS_CTRL) && code >= '0' && code <= '9') kq_push(K_CTRL_0 + (code - '0'), mods, NULL);
        else if ((mods & KEYS_CTRL) && ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')))
            kq_push(code & 0x1F, mods, NULL);
        else if (code >= 0x20 && code < 0x7F) kq_push(code, mods, NULL);
        return;
    }

    switch (fin) {
    case 'A': kq_push(K_UP, mods, NULL); return;
    case 'B': kq_push(K_DOWN, mods, NULL); return;
    case 'C': kq_push(K_RIGHT, mods, NULL); return;
    case 'D': kq_push(K_LEFT, mods, NULL); return;
    case 'H': kq_push(K_BOL, mods, NULL); return;
    case 'F': kq_push(K_EOL, mods, NULL); return;
    case 'Z': kq_push('\t', mods | KEYS_SHIFT, NULL); return;
    case 'P': kq_push(K_F1 + 0, mods, NULL); return;
    case 'Q': kq_push(K_F1 + 1, mods, NULL); return;
    case 'R': kq_push(K_F1 + 2, mods, NULL); return;
    case 'S': kq_push(K_F1 + 3, mods, NULL); return;
    case '~':
        switch (p[0]) {
        case 1: case 7: kq_push(K_BOL, mods, NULL); return;
        case 2: kq_push(K_INS, mods, NULL); return;
        case 3: kq_push(K_DEL, mods, NULL); return;
        case 4: case 8: kq_push(K_EOL, mods, NULL); return;
        case 5: kq_push(K_PGUP, mods, NULL); return;
        case 6: kq_push(K_PGDN, mods, NULL); return;
        case 11: kq_push(K_F1 + 0, mods, NULL); return;
        case 12: kq_push(K_F1 + 1, mods, NULL); return;
        case 13: kq_push(K_F1 + 2, mods, NULL); return;
        case 14: kq_push(K_F1 + 3, mods, NULL); return;
        case 15: kq_push(K_F1 + 4, mods, NULL); return;
        case 17: kq_push(K_F1 + 5, mods, NULL); return;
        case 18: kq_push(K_F1 + 6, mods, NULL); return;
        case 19: kq_push(K_F1 + 7, mods, NULL); return;
        case 20: kq_push(K_F1 + 8, mods, NULL); return;
        case 21: kq_push(K_F1 + 9, mods, NULL); return;
        case 23: kq_push(K_F1 + 10, mods, NULL); return;
        case 24: kq_push(K_F1 + 11, mods, NULL); return;
        }
        return;
    }
}

static void handle_ss3(uint8_t ch)
{
    switch (ch) {
    case 'A': kq_push(K_UP, 0, NULL); break;
    case 'B': kq_push(K_DOWN, 0, NULL); break;
    case 'C': kq_push(K_RIGHT, 0, NULL); break;
    case 'D': kq_push(K_LEFT, 0, NULL); break;
    case 'H': kq_push(K_BOL, 0, NULL); break;
    case 'F': kq_push(K_EOL, 0, NULL); break;
    case 'P': kq_push(K_F1 + 0, 0, NULL); break;
    case 'Q': kq_push(K_F1 + 1, 0, NULL); break;
    case 'R': kq_push(K_F1 + 2, 0, NULL); break;
    case 'S': kq_push(K_F1 + 3, 0, NULL); break;
    }
}

/* Parse as many complete tokens as possible from T.in_buf.
 * If flush_esc is 1, a lone trailing ESC is emitted as key 27. */
static void parse_input(int flush_esc)
{
    size_t pos = 0;
    while (pos < T.in_len) {
        uint8_t b = T.in_buf[pos];
        if (b == 0x1b) {
            if (pos + 1 >= T.in_len) {
                if (flush_esc) {
                    kq_push(27, 0, NULL);
                    pos += 1;
                    T.esc_since_ms = 0;
                    continue;
                }
                if (T.esc_since_ms == 0) T.esc_since_ms = now_ms();
                break;
            }
            T.esc_since_ms = 0;
            uint8_t b1 = T.in_buf[pos + 1];
            if (b1 == '[') {
                size_t j = pos + 2;
                while (j < T.in_len && !(T.in_buf[j] >= 0x40 && T.in_buf[j] <= 0x7e)) j++;
                if (j >= T.in_len) {
                    if (T.in_len - pos > 64) { pos += 2; continue; }
                    break; /* wait for rest of CSI */
                }
                handle_csi(T.in_buf + pos + 2, j - (pos + 2) + 1);
                pos = j + 1;
                continue;
            }
            if (b1 == 'O') {
                if (pos + 2 >= T.in_len) break;
                handle_ss3(T.in_buf[pos + 2]);
                pos += 3;
                continue;
            }
            if (b1 == '_' || b1 == 'P' || b1 == ']') {
                /* APC / DCS / OSC: skip until ST (ESC \) or BEL (for OSC). */
                size_t j = pos + 2;
                int found = 0;
                while (j < T.in_len) {
                    if (b1 == ']' && T.in_buf[j] == 0x07) { j++; found = 1; break; }
                    if (T.in_buf[j] == 0x1b && j + 1 < T.in_len && T.in_buf[j + 1] == '\\') {
                        j += 2; found = 1; break;
                    }
                    j++;
                }
                if (!found) {
                    if (T.in_len - pos > 256) { pos = T.in_len; break; }
                    break;
                }
                pos = j;
                continue;
            }
            /* Alt + key */
            if (b1 == 0x0d || b1 == 0x0a) {
                kq_push('\n', KEYS_ALT, NULL);
            } else if (b1 == 0x7f || b1 == 0x08) {
                kq_push('\b', KEYS_ALT, NULL);
            } else if (b1 >= 0x20 && b1 < 0x7f) {
                kq_push((int)b1, KEYS_ALT, NULL);
            } else if (b1 >= 1 && b1 <= 26) {
                kq_push((int)b1, KEYS_CTRL | KEYS_ALT, NULL);
            }
            pos += 2;
            continue;
        }

        T.esc_since_ms = 0;
        if (b == 0x00) {
            kq_push(K_CTRL_SPACE, KEYS_CTRL, NULL);
            pos++;
        } else if (b == 0x7f || b == 0x08) {
            kq_push('\b', 0, NULL);
            pos++;
        } else if (b == 0x09) {
            kq_push('\t', 0, NULL);
            pos++;
        } else if (b == 0x0a || b == 0x0d) {
            kq_push('\n', 0, NULL);
            pos++;
        } else if (b >= 1 && b <= 26) {
            kq_push((int)b, KEYS_CTRL, NULL);
            pos++;
        } else if (b >= 0x20 && b < 0x7f) {
            kq_push((int)b, 0, NULL);
            pos++;
        } else if ((b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0 || (b & 0xF8) == 0xF0) {
            size_t need = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
            if (pos + need > T.in_len) break; /* partial UTF-8 */
            int valid = 1;
            for (size_t k = 1; k < need; k++) {
                if ((T.in_buf[pos + k] & 0xC0) != 0x80) { valid = 0; break; }
            }
            if (!valid) { pos++; continue; }
            char u8[5];
            memcpy(u8, T.in_buf + pos, need);
            u8[need] = '\0';
            kq_push(K_TEXT, 0, u8);
            pos += need;
        } else {
            pos++;
        }
    }

    if (pos > 0) {
        if (pos < T.in_len) memmove(T.in_buf, T.in_buf + pos, T.in_len - pos);
        T.in_len -= pos;
    }
}

static void handle_resize_signal(void)
{
    int old_cols = T.term_cols, old_rows = T.term_rows;
    int old_pxw = T.px_w, old_pxh = T.px_h;
    query_winsize();
    if (tty_backend_pixel_mode()) {
        if (T.px_w <= 0 || T.px_h <= 0 || (T.px_w == old_pxw && T.px_h == old_pxh &&
                                           (T.term_cols != old_cols || T.term_rows != old_rows))) {
            /* Some terminals update TIOCGWINSZ cells but answer pixel size via CSI 14 t. */
            out_puts("\x1b[14t");
            out_flush();
        }
        apply_pixel_geometry();
    } else {
        apply_cell_geometry();
    }
}

int tty_backend_wait(struct pollfd *extra, size_t count, int timeout_ms)
{
    if (!T.active) return -1;
    for (size_t i = 0; i < count; i++) extra[i].revents = 0;

    /* Typeahead read during the capability probe (or left pending) is parsed
     * before anything else, so it is never lost behind the resize flag. */
    if (T.in_len > 0) parse_input(0);
    if (T.shown || T.kq_len > 0) return 1;
    if (T.eof) return -1;

    int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;

    for (;;) {
        if (T.shown || T.kq_len > 0) return 1;

        int wait_ms = timeout_ms;
        if (timeout_ms > 0) {
            int64_t left = deadline - now_ms();
            wait_ms = left > 0 ? (int)left : 0;
        }
        if (T.esc_since_ms > 0) {
            int64_t esc_left = (T.esc_since_ms + ESC_TIMEOUT_MS) - now_ms();
            if (esc_left <= 0) {
                parse_input(1);
                if (T.kq_len > 0) return 1;
                continue;
            }
            if (wait_ms < 0 || esc_left < wait_ms) wait_ms = (int)esc_left;
        }

        struct pollfd stack_pf[64];
        size_t base_n = (winch_pipe[0] >= 0) ? 2 : 1;
        size_t total = base_n + count;
        struct pollfd *pf = total <= 64 ? stack_pf : calloc(total, sizeof *pf);
        if (!pf) return 0;

        pf[0] = (struct pollfd){ .fd = T.in_fd, .events = POLLIN };
        int winch_idx = -1;
        if (winch_pipe[0] >= 0) {
            winch_idx = 1;
            pf[1] = (struct pollfd){ .fd = winch_pipe[0], .events = POLLIN };
        }
        for (size_t i = 0; i < count; i++) pf[base_n + i] = extra[i];

        int r = poll(pf, (nfds_t)total, wait_ms);
        int err = errno;

        short in_rev = pf[0].revents;
        short winch_rev = winch_idx >= 0 ? pf[winch_idx].revents : 0;
        int extra_ready = 0;
        if (r > 0) {
            for (size_t i = 0; i < count; i++) {
                extra[i].revents = pf[base_n + i].revents;
                if (extra[i].revents) extra_ready = 1;
            }
        }
        if (pf != stack_pf) free(pf);

        if (r < 0) {
            if (err == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            if (T.esc_since_ms > 0 && now_ms() >= T.esc_since_ms + ESC_TIMEOUT_MS) {
                parse_input(1);
                if (T.kq_len > 0) return 1;
            }
            if (timeout_ms == 0 || (timeout_ms > 0 && now_ms() >= deadline)) return 0;
            continue;
        }

        int tty_event = 0;
        T.mouse_ev = 0;
        if (winch_rev & POLLIN) {
            char drain[32];
            while (read(winch_pipe[0], drain, sizeof drain) > 0) {}
            handle_resize_signal();
            tty_event = 1;
        }
        if (in_rev & POLLIN) {
            if (T.in_len < IN_BUF_CAP) {
                ssize_t n = read(T.in_fd, T.in_buf + T.in_len, IN_BUF_CAP - T.in_len);
                if (n > 0) {
                    T.in_len += (size_t)n;
                    parse_input(0);
                    if (T.kq_len > 0 || T.mouse_ev || T.shown) tty_event = 1;
                } else if (n == 0) {
                    if (T.in_len > 0) parse_input(1);
                    T.eof = 1;
                    if (T.kq_len > 0 || T.mouse_ev) return 1;
                    return -1;
                }
            }
        } else if (in_rev & (POLLHUP | POLLERR | POLLNVAL)) {
            if (T.in_len > 0) parse_input(1);
            T.eof = 1;
            if (T.kq_len > 0 || T.mouse_ev) return 1;
            return -1;
        }

        if (T.shown || T.kq_len > 0 || tty_event) return 1;
        if (extra_ready) return 2;
        if (timeout_ms == 0 || (timeout_ms > 0 && now_ms() >= deadline)) return 0;
    }
}

int tty_backend_shown(void)
{
    int s = T.shown;
    T.shown = 0;
    return s;
}

int tty_backend_next_key(void)
{
    if (T.kq_len == 0) return 0;
    struct tty_key_ev ev = T.kq[T.kq_head];
    T.kq_head = (T.kq_head + 1) % KEY_QUEUE_CAP;
    T.kq_len--;
    T.last_mods = ev.mods;
    memcpy(T.last_text, ev.text, sizeof T.last_text);
    return ev.key;
}

const char *tty_backend_text(void) { return T.last_text; }
int tty_backend_modifiers(void) { return T.last_mods; }

/* --- OSC 52 Clipboard --- */

int tty_backend_clipboard_set(const char *text, size_t len)
{
    if (!T.active || T.out_fd < 0 || !text) return -1;
    if (len > OSC52_MAX_BYTES) len = OSC52_MAX_BYTES;
    out_puts("\x1b]52;c;");
    out_base64_chunk((const uint8_t *)text, len);
    out_puts("\x07");
    out_flush();
    return 0;
}

/* --- Suspend / Resume for interactive subcommands --- */

int tty_backend_run(char *const argv[])
{
    if (!T.active || !argv || !argv[0]) return -1;

    sig_active = 0;
    emit_leave_terminal();
    restore_cooked_termios();

    struct sigaction old_winch, old_int, old_quit, ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    /* Ctrl+\ belongs to the subcommand; this program keeps waiting and repaints. */
    sigaction(SIGQUIT, &ign, &old_quit);
    if (!T.custom_fds) {
        sigaction(SIGINT, &ign, &old_int);
        sigaction(SIGWINCH, &ign, &old_winch);
    }

    pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGQUIT, &old_quit, NULL);
        if (!T.custom_fds) {
            sigaction(SIGINT, &old_int, NULL);
            sigaction(SIGWINCH, &old_winch, NULL);
        }
        enter_raw_termios();
        emit_enter_terminal();
        if (!T.custom_fds) sig_active = 1;
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGWINCH, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (T.custom_fds) {
            if (T.in_fd != STDIN_FILENO) dup2(T.in_fd, STDIN_FILENO);
            if (T.out_fd != STDOUT_FILENO) dup2(T.out_fd, STDOUT_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0, result = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid) {
            result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }

    if (!T.custom_fds) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGWINCH, &old_winch, NULL);
    }
    sigaction(SIGQUIT, &old_quit, NULL);

    enter_raw_termios();
    emit_enter_terminal();
    if (!T.custom_fds) sig_active = 1;
    T.in_len = 0;
    T.esc_since_ms = 0;
    query_winsize();
    if (tty_backend_pixel_mode()) {
        apply_pixel_geometry();
    } else {
        apply_cell_geometry();
    }
    return result;
}
#endif
