/* The terminal transport for LOCachy programs (tty-backend.c), used over SSH
 * or inside a PTY when neither process 1 (LOCACHY_CONTROL) nor a Wayland window
 * (LOCACHY_BACKEND=wayland) provides the screen.
 *
 * Three rendering tiers are probed in-band on open (working even through sudo):
 *   1. Kitty Graphics Protocol (Ghostty, Kitty, WezTerm, Konsole) — reuses
 *      screen.c's exact XRGB8888 software framebuffer (shadow + present_diff)
 *      with dirty horizontal row strips and HiDPI scale detection.
 *   2. Sixel Graphics (iTerm2, foot, Windows Terminal, xterm) — also reuses
 *      screen.c's exact software framebuffer with adaptive palette + RLE.
 *   3. ANSI Cell Fallback (macOS Terminal.app, tmux without passthrough, plain
 *      SSH) — crisp UTF-8 cell grid with TrueColor / 256-color SGR diffs.
 *
 * Inside the base and host admin programs only. Direct-only builds (process 1's
 * static musl init) compile none of it. */
#ifndef LOCACHY_TTY_BACKEND_H
#define LOCACHY_TTY_BACKEND_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TTY_MODE_NONE = 0,
    TTY_MODE_PIXEL_KITTY = 1,
    TTY_MODE_PIXEL_SIXEL = 2,
    TTY_MODE_CELL = 3,
};

/* Weak hooks so core modules (control.c, clipboard.c) and programs link cleanly
 * whether or not the TTY backend is compiled in (e.g. musl init, unit tests). */
__attribute__((weak)) int tty_backend_wanted(void);
__attribute__((weak)) int tty_backend_active(void);
__attribute__((weak)) int tty_backend_run(char *const argv[]);
__attribute__((weak)) void tty_backend_close(void);
__attribute__((weak)) int tty_backend_clipboard_set(const char *text, size_t len);

#ifdef CONFIG_TTY
/* 1 if LOCACHY_BACKEND explicitly asks for "tty", 0 otherwise. */
int tty_backend_explicit(void);
/* 1 if the TTY backend should be used (explicit LOCACHY_BACKEND=tty, or auto
 * on an interactive PTY / SSH session with no PID 1 control socket). */
int tty_backend_wanted(void);
/* Prevent auto-TTY selection (for physical-display programs: extdisplay, lock). */
void tty_backend_forbid_auto(void);
/* Active mode: TTY_MODE_NONE (0) when closed, else PIXEL_KITTY / PIXEL_SIXEL / CELL. */
int tty_backend_active(void);
int tty_backend_pixel_mode(void);
int tty_backend_cell_mode(void);

/* Open the terminal transport, probe capabilities, and initialize either the
 * pixel framebuffer (calling resize(w, h, scale)) or the cell grid (calling
 * cell_resize(cols, rows)). Returns 0 on success, -1 on failure. */
int tty_backend_open(int (*resize)(int width, int height, double scale),
                     int (*cell_resize)(int cols, int rows));
void tty_backend_close(void);

/* Pixel framebuffer flush (Kitty / Sixel modes): present the changed rectangle
 * [x0, x1) x [y0, y1) of the w x h XRGB8888 canvas. Returns 0 on success. */
int tty_backend_flush_pixels(const uint32_t *pixels, int w, int h, int x0, int y0, int x1, int y1);

/* Cell-grid drawing primitives (ANSI Cell fallback mode). */
int tty_cell_cols(void);
int tty_cell_rows(void);
void tty_cell_fill(int col, int row, int ncols, int nrows, uint32_t rgb);
void tty_cell_text(int col, int row, const char *s, uint32_t fg, uint32_t bg);
void tty_cell_bar(const char *title, const char *ime);
void tty_cell_flush(void);

/* Wait at most timeout_ms (-1 forever, 0 poll) for terminal input, SIGWINCH, or
 * extra fds. Returns 1 on terminal event, 2 when an extra fd is ready, 0 on
 * timeout, -1 when stdin reached EOF/HUP. */
int tty_backend_wait(struct pollfd *extra, size_t count, int timeout_ms);
int tty_backend_shown(void);
int tty_backend_next_key(void);
const char *tty_backend_text(void);
int tty_backend_modifiers(void);

/* Suspend the TUI (leave alt-screen, restore cooked termios), run argv directly
 * on the controlling terminal, then re-enter raw + alt-screen and schedule a
 * full repaint. Returns WEXITSTATUS or -1. */
int tty_backend_run(char *const argv[]);

/* Send clipboard text to the remote terminal via OSC 52. Returns 0 on success. */
int tty_backend_clipboard_set(const char *text, size_t len);

/* Test hook: bypass stdio/termios/signal handlers with custom fds and geometry.
 * force_mode: 0 = run normal probe on in_fd/out_fd, or TTY_MODE_PIXEL_KITTY /
 * TTY_MODE_PIXEL_SIXEL / TTY_MODE_CELL to skip probe and force that mode. */
void tty_backend_use_fds(int in_fd, int out_fd, int cols, int rows,
                         int xpixel, int ypixel, int force_mode);
/* Test hook: reset custom fd override back to normal stdio. */
void tty_backend_reset_fds(void);
#endif

#endif
