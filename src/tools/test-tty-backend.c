/* Unit tests for base/tty-backend.c:
 *   1. In-band capability probe (Kitty graphics + CSI u, Sixel DA1, ANSI cell fallback, typeahead).
 *   2. Kitty graphics pixel framebuffer flush (dirty cell-row strip, ping-pong IDs, zlib).
 *   3. Sixel graphics pixel framebuffer flush (adaptive exact palette + >256-color fallback).
 *   4. ANSI cell grid diff renderer (TrueColor, xterm-256, wide CJK orphan cleanup, no-op diff).
 *   5. Keyboard & SGR mouse decoding (ASCII, UTF-8, Ctrl/Alt/Shift, Esc timeout, CSI/SS3, CSI u, mouse).
 *   6. OSC 52 clipboard and tty_backend_run() suspend/resume. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base.h"
#include "tty-backend.h"

static int got_px_w, got_px_h, got_cell_c, got_cell_r;
static double got_scale;
static int ptr_x, ptr_y, ptr_btn, ptr_pressed, ptr_present;
static int scroll_src, scroll_axes;
static double scroll_v, scroll_h;

void pointer_window_motion(int x, int y) { ptr_x = x; ptr_y = y; }
void pointer_window_button(int code, int pressed) { ptr_btn = code; ptr_pressed = pressed; }
void pointer_window_scroll(int source, double v, double h, int axes)
{
    scroll_src = source;
    scroll_v = v;
    scroll_h = h;
    scroll_axes = axes;
}
void pointer_window_present(int on) { ptr_present = on; }
void pointer_window_leave(void) {}
int control_fd(void) { return -1; }

static int on_px_resize(int w, int h, double s)
{
    got_px_w = w;
    got_px_h = h;
    got_scale = s;
    return 0;
}

static int on_cell_resize(int c, int r)
{
    got_cell_c = c;
    got_cell_r = r;
    return 0;
}

static size_t drain_fd(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, 20) <= 0 || !(pf.revents & POLLIN)) break;
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= cap - 1) break;
    }
    buf[total] = '\0';
    return total;
}

static void write_str(int fd, const char *s)
{
    size_t n = strlen(s);
    ssize_t w = write(fd, s, n);
    assert(w == (ssize_t)n);
}

static void make_pipes(int in_p[2], int out_p[2])
{
    assert(pipe2(in_p, O_CLOEXEC) == 0);
    assert(pipe2(out_p, O_CLOEXEC) == 0);
    assert(fcntl(out_p[0], F_SETFL, O_NONBLOCK) == 0);
}

static void close_pipes(int in_p[2], int out_p[2])
{
    close(in_p[0]);
    close(in_p[1]);
    close(out_p[0]);
    close(out_p[1]);
}

static void test_probes(void)
{
    int in_p[2], out_p[2];
    char out[16384];

    /* 1a. Ghostty/Kitty: Kitty gfx OK + CSI u + pixel size CSI 4;720;1280t + DA1 */
    unsetenv("LOCACHY_TTY_MODE");
    make_pipes(in_p, out_p);
    write_str(in_p[1], "\x1b_Gi=31;OK\x1b\\\x1b[?0u\x1b[4;720;1280t\x1b[?62;22c");
    got_px_w = got_px_h = 0;
    got_scale = 0;
    tty_backend_use_fds(in_p[0], out_p[1], 80, 24, 0, 0, 0);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    assert(tty_backend_active() == TTY_MODE_PIXEL_KITTY);
    assert(got_px_w == 1280 && got_px_h == 720);
    assert(got_scale > 1.70 && got_scale < 1.80); /* 30/18 -> 1.75 */
    assert(ptr_present == 1);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[>1u") != NULL);
    assert(strstr(out, "\x1b[?1016h") != NULL);
    tty_backend_close();
    assert(ptr_present == 0);
    close_pipes(in_p, out_p);
    printf("PASS probe selects Kitty pixel framebuffer + CSI u + HiDPI scale\n");

    /* 1b. iTerm2/foot: Sixel attribute 4 in DA1 with pixel winsize */
    make_pipes(in_p, out_p);
    write_str(in_p[1], "\x1b[?62;4;22c");
    got_px_w = got_px_h = 0;
    tty_backend_use_fds(in_p[0], out_p[1], 80, 24, 960, 432, 0);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    assert(tty_backend_active() == TTY_MODE_PIXEL_SIXEL);
    assert(got_px_w == 960 && got_px_h == 432);
    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS probe selects Sixel pixel framebuffer on DA1 attribute 4\n");

    /* 1c. Plain terminal (macOS Terminal.app): no Kitty/Sixel, preserves typeahead after DA1 */
    make_pipes(in_p, out_p);
    write_str(in_p[1], "\x1b[?1;2cabc");
    got_cell_c = got_cell_r = 0;
    tty_backend_use_fds(in_p[0], out_p[1], 80, 24, 0, 0, 0);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    assert(tty_backend_active() == TTY_MODE_CELL);
    assert(got_cell_c == 80 && got_cell_r == 24);
    assert(tty_backend_shown() == 1);
    assert(tty_backend_wait(NULL, 0, 50) == 1); /* typeahead "abc" parsed from in_buf */
    assert(tty_backend_next_key() == 'a');
    assert(tty_backend_next_key() == 'b');
    assert(tty_backend_next_key() == 'c');
    assert(tty_backend_next_key() == 0);
    write_str(in_p[1], "d");
    assert(tty_backend_wait(NULL, 0, 200) == 1);
    assert(tty_backend_next_key() == 'd');
    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS probe falls back to ANSI cell mode and preserves typeahead\n");
}

static void test_kitty_and_sixel_flush(void)
{
    int in_p[2], out_p[2];
    static char out[131072];
    const int w = 320, h = 216; /* 40x12 cells, 8x18 px */
    uint32_t *fb = calloc((size_t)w * (size_t)h, sizeof *fb);
    assert(fb != NULL);
    for (int i = 0; i < w * h; i++) fb[i] = 0x101820u;

    /* Kitty dirty strip + ping-pong ID replacement */
    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 40, 12, w, h, TTY_MODE_PIXEL_KITTY);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    drain_fd(out_p[0], out, sizeof out);

    assert(tty_backend_flush_pixels(fb, w, h, 0, 18, w, 36) == 0);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[?2026h") != NULL);
    assert(strstr(out, "\x1b[2;1H") != NULL);
    assert(strstr(out, "\x1b[1;1H") == NULL);
    assert(strstr(out, "\x1b[3;1H") == NULL);
    assert(strstr(out, "a=T,f=24,t=d,i=11,p=1,s=320,v=18,C=1,q=2") != NULL);
    assert(strstr(out, "\x1b[?2026l") != NULL);

    /* Second flush of same row must use ping-pong ID 11+512=523 and delete old ID 11 */
    assert(tty_backend_flush_pixels(fb, w, h, 0, 18, w, 36) == 0);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "i=523,p=1,s=320,v=18") != NULL);
    assert(strstr(out, "\x1b_Ga=d,d=I,i=11,q=2\x1b\\") != NULL);

    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS Kitty pixel flush sends dirty cell-row strip and ping-pong IDs\n");

    /* Sixel: lossless small palette + >256-color fallback */
    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 40, 12, w, h, TTY_MODE_PIXEL_SIXEL);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    drain_fd(out_p[0], out, sizeof out);

    assert(tty_backend_flush_pixels(fb, w, h, 0, 18, w, 36) == 0);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[?2026h") != NULL);
    assert(strstr(out, "\x1b[2;1H\x1bP0;1;0q\"1;1;320;18") != NULL);
    assert(strstr(out, "#0;2;") != NULL);
    assert(strstr(out, "!") != NULL);
    assert(strstr(out, "\x1b\\\x1b[?2026l") != NULL);

    /* Fill row 0 with >300 unique colors to exercise 6x6x6 cube + greyscale fallback */
    for (int y = 0; y < 18; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * w + x] = ((uint32_t)((x * 255) / w) << 16) |
                            ((uint32_t)((y * 255) / 18) << 8) |
                            (uint32_t)(((x + y) * 7) & 0xFFu);
        }
    }
    assert(tty_backend_flush_pixels(fb, w, h, 0, 0, w, 18) == 0);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[1;1H\x1bP0;1;0q\"1;1;320;18") != NULL);
    assert(strstr(out, "\x1b\\\x1b[?2026l") != NULL);

    tty_backend_close();
    close_pipes(in_p, out_p);
    free(fb);
    printf("PASS Sixel pixel flush handles exact palette and >256-color fallback\n");
}

static void test_cell_rendering(void)
{
    int in_p[2], out_p[2];
    char out[32768];

    /* TrueColor diff + CJK width + no-op second flush */
    setenv("COLORTERM", "truecolor", 1);
    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 40, 10, 0, 0, TTY_MODE_CELL);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    drain_fd(out_p[0], out, sizeof out);

    tty_cell_fill(0, 0, 40, 10, 0x101820u);
    tty_cell_bar("settings", NULL);
    tty_cell_text(2, 2, "Hello 中文!", 0xFFFFFFu, 0x101820u);
    tty_cell_flush();
    size_t n1 = drain_fd(out_p[0], out, sizeof out);
    assert(n1 > 0);
    assert(strstr(out, "LOCachy") != NULL);
    assert(strstr(out, "settings") != NULL);
    assert(strstr(out, "Hello") != NULL);
    assert(strstr(out, "中文") != NULL);
    assert(strstr(out, "38;2;255;255;255m") != NULL);
    assert(strstr(out, "48;2;16;24;32m") != NULL);

    /* Unchanged second flush writes 0 bytes */
    tty_cell_flush();
    assert(drain_fd(out_p[0], out, sizeof out) == 0);

    /* Overwriting second half of wide CJK char clears orphan first half */
    tty_cell_text(9, 2, "X", 0xFFFFFFu, 0x101820u); /* col 8-9 is '中' */
    tty_cell_flush();
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[3;9H") != NULL);
    assert(strstr(out, "X") != NULL);

    tty_backend_close();
    close_pipes(in_p, out_p);

    /* xterm-256color fallback when COLORTERM is absent */
    unsetenv("COLORTERM");
    setenv("TERM", "xterm-256color", 1);
    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 40, 10, 0, 0, TTY_MODE_CELL);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    drain_fd(out_p[0], out, sizeof out);
    tty_cell_text(1, 1, "Hi", 0x80C0FFu, 0x101820u);
    tty_cell_flush();
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "38;5;") != NULL);
    assert(strstr(out, "48;5;") != NULL);
    assert(strstr(out, "38;2;") == NULL);
    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS ANSI cell diff renderer handles TrueColor, xterm-256, CJK and zero-diff\n");
}

static int next_key_wait(void)
{
    int k = tty_backend_next_key();
    if (k) return k;
    int r = tty_backend_wait(NULL, 0, 120);
    assert(r == 1);
    return tty_backend_next_key();
}

static void test_input_clipboard_and_run(void)
{
    int in_p[2], out_p[2];
    char out[16384];

    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 80, 24, 0, 0, TTY_MODE_CELL);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    assert(tty_backend_shown() == 1);
    drain_fd(out_p[0], out, sizeof out);

    /* Basic keys, Ctrl, UTF-8, CSI/SS3, Kitty CSI u, Alt+key */
    const char seq[] =
        "a\r\n\t\x1b[Z\x7f\x08"
        "\x11\x03\x01"
        "中"
        "\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[5~\x1b[6~\x1b[2~\x1b[3~"
        "\x1bOP\x1b[24~"
        "\x1b[1;3D\x1bx"
        "\x1b[27u\x1b[113;5u\x1b[51;5u\x1b[97;1:3u";
    ssize_t w = write(in_p[1], seq, sizeof seq - 1);
    assert(w == (ssize_t)(sizeof seq - 1));
    /* Also write NUL (Ctrl+Space) */
    char nul = 0;
    assert(write(in_p[1], &nul, 1) == 1);

    assert(next_key_wait() == 'a' && tty_backend_modifiers() == 0);
    assert(next_key_wait() == '\n');
    assert(next_key_wait() == '\n');
    assert(next_key_wait() == '\t' && tty_backend_modifiers() == 0);
    assert(next_key_wait() == '\t' && key_action('\t', tty_backend_modifiers()) == ACT_PREV_FIELD);
    assert(next_key_wait() == '\b');
    assert(next_key_wait() == '\b');
    int k = next_key_wait();
    assert(key_action(k, tty_backend_modifiers()) == ACT_QUIT);
    k = next_key_wait();
    assert(key_action(k, tty_backend_modifiers()) == ACT_COPY);
    k = next_key_wait();
    assert(key_action(k, tty_backend_modifiers()) == ACT_SELECT_ALL);
    assert(next_key_wait() == K_TEXT && strcmp(tty_backend_text(), "中") == 0);
    assert(next_key_wait() == K_UP);
    assert(next_key_wait() == K_DOWN);
    assert(next_key_wait() == K_RIGHT);
    assert(next_key_wait() == K_LEFT);
    assert(next_key_wait() == K_BOL);
    assert(next_key_wait() == K_EOL);
    assert(next_key_wait() == K_PGUP);
    assert(next_key_wait() == K_PGDN);
    assert(next_key_wait() == K_INS);
    assert(next_key_wait() == K_DEL);
    assert(next_key_wait() == K_F1);
    assert(next_key_wait() == K_F1 + 11);
    k = next_key_wait();
    assert(k == K_LEFT && key_action(k, tty_backend_modifiers()) == ACT_BACK);
    assert(next_key_wait() == 'x' && tty_backend_modifiers() == KEYS_ALT);
    /* Kitty CSI u: Esc, Ctrl+Q, Ctrl+3 (release event 97;1:3u must be skipped) */
    k = next_key_wait();
    assert(k == 27 && key_action(k, tty_backend_modifiers()) == ACT_CANCEL);
    k = next_key_wait();
    assert(key_action(k, tty_backend_modifiers()) == ACT_QUIT);
    assert(next_key_wait() == K_CTRL_0 + 3);
    assert(next_key_wait() == K_CTRL_SPACE);

    /* Lone ESC disambiguation via 40 ms timeout */
    write_str(in_p[1], "\x1b");
    k = next_key_wait();
    assert(k == 27 && tty_backend_modifiers() == 0);

    /* SGR mouse click + release + wheel */
    ptr_x = ptr_y = ptr_btn = ptr_pressed = 0;
    scroll_src = scroll_axes = 0;
    scroll_v = scroll_h = 0;
    write_str(in_p[1], "\x1b[<0;120;240M\x1b[<0;120;240m\x1b[<64;10;20M");
    assert(tty_backend_wait(NULL, 0, 100) == 1);
    assert(ptr_x == 9 && ptr_y == 19); /* last motion was wheel at (10,20) -> 0-based (9,19) */
    assert(ptr_btn == BTN_LEFT && ptr_pressed == 0);
    assert(scroll_src == SCROLL_WHEEL && scroll_v < 0.0 && scroll_axes == 1);

    /* OSC 52 clipboard */
    drain_fd(out_p[0], out, sizeof out);
    assert(tty_backend_clipboard_set("hello", 5) == 0);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b]52;c;aGVsbG8=\x07") != NULL);

    /* Suspend/resume subcommand via tty_backend_run */
    char *cmd[] = { "/bin/sh", "-c", "exit 42", NULL };
    assert(tty_backend_run(cmd) == 42);
    drain_fd(out_p[0], out, sizeof out);
    assert(strstr(out, "\x1b[?1049l") != NULL);
    assert(strstr(out, "\x1b[?1049h") != NULL);
    assert(tty_backend_shown() == 1);

    /* Ctrl+\ from the subcommand (SIGQUIT to this process) does not end the program */
    char *quit_cmd[] = { "/bin/sh", "-c", "kill -QUIT $PPID; exit 0", NULL };
    assert(tty_backend_run(quit_cmd) == 0);

    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS keyboard, mouse, Esc timeout, OSC 52 clipboard and tty_backend_run\n");
}

static void test_cell_sanitize(void)
{
    int in_p[2], out_p[2];
    static char out[32768];

    /* Raw and UTF-8 C1 controls, and bytes that are not UTF-8, never reach the terminal */
    make_pipes(in_p, out_p);
    tty_backend_use_fds(in_p[0], out_p[1], 40, 10, 0, 0, TTY_MODE_CELL);
    assert(tty_backend_open(on_px_resize, on_cell_resize) == 0);
    drain_fd(out_p[0], out, sizeof out);

    /* Raw/UTF-8 C1 controls, overlong UTF-8, surrogates, >U+10FFFF, and raw invalid bytes */
    tty_cell_text(1, 1,
                  "a\x9b" "b\xc2\x9b" "c\xff" "d\xc1\x81" "e\xed\xa0\x80" "f\xf5\x80\x80\x80",
                  0xFFFFFFu, 0x101820u);
    tty_cell_flush();
    size_t n = drain_fd(out_p[0], out, sizeof out);
    assert(n > 0);
    assert(memchr(out, 0x9b, n) == NULL);
    assert(memchr(out, 0xff, n) == NULL);
    assert(memchr(out, 0xc1, n) == NULL);
    assert(memchr(out, 0xed, n) == NULL);
    assert(memchr(out, 0xf5, n) == NULL);
    assert(strstr(out, "\xef\xbf\xbd") != NULL);

    tty_backend_close();
    close_pipes(in_p, out_p);
    printf("PASS cell renderer keeps C1 controls and invalid UTF-8 off the terminal\n");
}

int main(void)
{
    test_probes();
    test_kitty_and_sixel_flush();
    test_cell_rendering();
    test_cell_sanitize();
    test_input_clipboard_and_run();
    tty_backend_reset_fds();
    printf("test-tty-backend: all checks passed.\n");
    return 0;
}
