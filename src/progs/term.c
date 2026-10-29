/*
 * term [PROGRAM ARG...]: a terminal for LOCachy (TERM=linux). It opens a
 * pseudo-terminal sized to the screen, and runs PROGRAM ARG... in it, or
 * /bin/bash -l when given none. The shell and all it starts live in this
 * program's cgroup: Ctrl+Alt+Esc freezes the whole session and pushes its
 * memory to swap; opening it again from home continues where it was. When
 * the child exits, term exits with its status. Lines scrolled off the top
 * are kept (HISTORY of them) to scroll back to with the wheel or Shift+PgUp.
 */
#define _XOPEN_SOURCE 600
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <poll.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "base.h"
#include "ime.h"
#include "clipboard.h"

#define DEF_FG 0xD0D0D0u
#define DEF_BG 0x101820u
#define MAX_PARAMS 16

static const uint32_t palette[16] = {
    0x101820, 0xCC4444, 0x44B866, 0xCCAA44, 0x4488CC, 0xB855B8, 0x44B8B8, 0xD0D0D0,
    0x606870, 0xFF6666, 0x66E088, 0xFFE066, 0x66B0FF, 0xE080E0, 0x66E0E0, 0xFFFFFF,
};

struct cell {
    uint32_t ch, fg, bg;
};

static struct cell *grid, *alt_grid;
static uint8_t *dirty, *soft, *alt_soft;
static int bracketed_paste, mouse_sgr;
/* Positions are row * cols + col of what is shown (history when scrolled back). */
static struct { int active, dragging, anchor, end; } selection;
#define HISTORY 10000
static struct cell *history;   /* a ring of HISTORY lines, hist_cols cells each */
static uint8_t *history_soft;
static int hist_cols, hist_len, hist_head, view; /* view: lines scrolled back */
static struct { char *data; size_t offset, size; } output;
#define OUTPUT_LIMIT (64u * 1024u * 1024u)

static void selection_clear(void)
{
    selection.active = selection.dragging = 0;
    /* The caller marks affected rows dirty after changing the selection. */
}

static int queue_output(const void *data, size_t size)
{
    size_t pending = output.size - output.offset;
    if (size > OUTPUT_LIMIT - pending) return -1;
    if (output.offset) memmove(output.data, output.data + output.offset, pending);
    output.offset = 0;
    output.size = pending;
    char *p = realloc(output.data, pending + size);
    if (!p && size) return -1;
    output.data = p;
    if (size) memcpy(p + pending, data, size);
    output.size += size;
    return 0;
}

static int flush_output(int fd)
{
    while (output.offset < output.size) {
        ssize_t n = write(fd, output.data + output.offset, output.size - output.offset);
        if (n > 0) output.offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else return n < 0 && errno == EAGAIN ? 0 : -1;
    }
    free(output.data);
    memset(&output, 0, sizeof output);
    return 0;
}
static int cols, rows;
static int cx, cy, top, bot, wrap, autowrap = 1, cursor_vis = 1;
static uint32_t cur_fg = DEF_FG, cur_bg = DEF_BG;
static int bold, rev;
static int saved_cx, saved_cy, alt_cx, alt_cy, in_alt;
static uint32_t saved_fg = DEF_FG, saved_bg = DEF_BG;
static int saved_bold, saved_rev;

static enum { ST_NORM, ST_ESC, ST_CSI, ST_OSC, ST_OSC_P, ST_CHARSET } state;
static int csi_priv, param[MAX_PARAMS], nparam, osc_n;
static uint32_t utf_cp;
static int utf_need;

/* Transient HUD toast for IME mode changes (disappears after ~0.8s) */
static char toast_msg[64];
static double toast_expire;
static int toast_active;
static int toast_col, toast_row, toast_w;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int is_wide(uint32_t cp)
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

static uint32_t eff_fg(void)
{
    return rev ? cur_bg : cur_fg;
}

static uint32_t eff_bg(void)
{
    return rev ? cur_fg : cur_bg;
}

static struct cell blank(void)
{
    return (struct cell){ ' ', eff_fg(), eff_bg() };
}

static void clear_span(int r, int c0, int c1)
{
    struct cell b = blank();

    if (r < 0 || r >= rows) {
        return;
    }
    c0 = clamp(c0, 0, cols);
    c1 = clamp(c1, 0, cols);
    for (int c = c0; c < c1; c++) {
        grid[r * cols + c] = b;
    }
    dirty[r] = 1;
    selection_clear();
    if (soft) soft[r] = 0;
}

/* A line leaving the top of the main screen goes into the history. */
static void keep_line(int r)
{
    if (!history) {
        history = calloc((size_t)HISTORY * (size_t)cols, sizeof *history);
        history_soft = calloc(HISTORY, 1);
        if (!history || !history_soft) {
            free(history); free(history_soft);
            history = NULL; history_soft = NULL;
            return;
        }
        hist_cols = cols;
    }
    int slot = (hist_head + hist_len) % HISTORY;
    if (hist_len == HISTORY) {
        hist_head = (hist_head + 1) % HISTORY;
    } else {
        hist_len++;
    }
    int keep = cols < hist_cols ? cols : hist_cols;
    for (int c = 0; c < hist_cols; c++) {
        history[(size_t)slot * hist_cols + c] = c < keep ? grid[r * cols + c] : (struct cell){ ' ', DEF_FG, DEF_BG };
    }
    history_soft[slot] = soft ? soft[r] : 0;
    if (view > 0 && view < hist_len) view++; /* what is looked at stays */
}

static void scroll_up(int r0, int r1, int n)
{
    if (n <= 0 || r0 > r1) {
        return;
    }
    if (n > r1 - r0 + 1) {
        n = r1 - r0 + 1;
    }
    selection_clear();
    if (r0 == 0 && !in_alt) {
        for (int r = 0; r < n; r++) keep_line(r);
    }
    if (soft) memmove(soft + r0, soft + r0 + n, (size_t)(r1 + 1 - r0 - n));
    if (r1 + 1 - r0 > n) {
        memmove(&grid[r0 * cols], &grid[(r0 + n) * cols],
                sizeof *grid * (size_t)((r1 + 1 - r0 - n) * cols));
    }
    for (int r = r1 + 1 - n; r <= r1; r++) {
        clear_span(r, 0, cols);
    }
    for (int r = r0; r <= r1; r++) {
        dirty[r] = 1;
    }
}

static void scroll_down(int r0, int r1, int n)
{
    if (n <= 0 || r0 > r1) {
        return;
    }
    if (n > r1 - r0 + 1) {
        n = r1 - r0 + 1;
    }
    selection_clear();
    if (soft) memmove(soft + r0 + n, soft + r0, (size_t)(r1 + 1 - r0 - n));
    if (r1 + 1 - r0 > n) {
        memmove(&grid[(r0 + n) * cols], &grid[r0 * cols],
                sizeof *grid * (size_t)((r1 + 1 - r0 - n) * cols));
    }
    for (int r = r0; r < r0 + n; r++) {
        clear_span(r, 0, cols);
    }
    for (int r = r0; r <= r1; r++) {
        dirty[r] = 1;
    }
}

static void newline(void)
{
    wrap = 0;
    if (cy == bot) {
        scroll_up(top, bot, 1);
    } else if (cy < rows - 1) {
        cy++;
    }
}

static void put_cp(uint32_t cp)
{
    selection_clear();
    if (wrap) {
        if (soft) soft[cy] = 1;
        cx = 0;
        newline();
    }
    if (is_wide(cp) && cx == cols - 1 && autowrap) {
        if (soft) soft[cy] = 1;
        cx = 0;
        newline();
    }
    grid[cy * cols + cx] = (struct cell){ cp, eff_fg(), eff_bg() };
    dirty[cy] = 1;
    int wide = is_wide(cp);
    if (wide && cx < cols - 1) {
        cx++;
        grid[cy * cols + cx] = (struct cell){ 0, eff_fg(), eff_bg() }; /* wide placeholder */
    }
    if (cx < cols - 1) {
        cx++;
    } else if (autowrap) {
        wrap = 1;
    }
}

static uint32_t color256(int n)
{
    if (n < 16) {
        return palette[n & 15];
    }
    if (n < 232) {
        int i = n - 16, r = i / 36, g = (i / 6) % 6, b = i % 6;
        uint32_t v[6] = { 0, 95, 135, 175, 215, 255 };
        return (v[r] << 16) | (v[g] << 8) | v[b];
    }
    uint32_t gray = (uint32_t)(8 + (n - 232) * 10) & 0xFF;
    return (gray << 16) | (gray << 8) | gray;
}

static void sgr(void)
{
    if (nparam == 0) {
        param[nparam++] = 0;
    }
    for (int i = 0; i < nparam; i++) {
        int p = param[i];
        if (p == 0) {
            cur_fg = DEF_FG;
            cur_bg = DEF_BG;
            bold = rev = 0;
        } else if (p == 1) {
            bold = 1;
            for (int k = 0; k < 8; k++) {
                if (cur_fg == palette[k]) {
                    cur_fg = palette[k + 8];
                }
            }
        } else if (p == 2 || p == 22) {
            bold = 0;
        } else if (p == 7) {
            rev = 1;
        } else if (p == 27) {
            rev = 0;
        } else if (p >= 30 && p <= 37) {
            cur_fg = palette[(p - 30) + (bold ? 8 : 0)];
        } else if (p == 38 && i + 2 < nparam && param[i + 1] == 5) {
            cur_fg = color256(param[i + 2]);
            i += 2;
        } else if (p == 38 && i + 4 < nparam && param[i + 1] == 2) {
            cur_fg = ((uint32_t)(param[i + 2] & 0xFF) << 16) |
                     ((uint32_t)(param[i + 3] & 0xFF) << 8) | (uint32_t)(param[i + 4] & 0xFF);
            i += 4;
        } else if (p == 39) {
            cur_fg = DEF_FG;
        } else if (p >= 40 && p <= 47) {
            cur_bg = palette[p - 40];
        } else if (p == 48 && i + 2 < nparam && param[i + 1] == 5) {
            cur_bg = color256(param[i + 2]);
            i += 2;
        } else if (p == 48 && i + 4 < nparam && param[i + 1] == 2) {
            cur_bg = ((uint32_t)(param[i + 2] & 0xFF) << 16) |
                     ((uint32_t)(param[i + 3] & 0xFF) << 8) | (uint32_t)(param[i + 4] & 0xFF);
            i += 4;
        } else if (p == 49) {
            cur_bg = DEF_BG;
        } else if (p >= 90 && p <= 97) {
            cur_fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            cur_bg = palette[p - 100 + 8];
        }
    }
}

static void reset_term(void)
{
    cx = cy = 0;
    top = 0;
    bot = rows - 1;
    wrap = 0;
    autowrap = cursor_vis = 1;
    cur_fg = DEF_FG;
    cur_bg = DEF_BG;
    bold = rev = 0;
    for (int r = 0; r < rows; r++) {
        clear_span(r, 0, cols);
    }
}

/* The screen's cells changed (its scale did): keep what fits, with the
 * cursor's row in view, and tell the program here (TIOCSWINSZ: SIGWINCH). */
static void resize(int pty)
{
    int ncols = screen_cols(), nrows = screen_rows() - 1;
    if ((ncols == cols && nrows == rows) || ncols <= 0 || nrows <= 0) {
        return;
    }
    size_t cells = (size_t)ncols * (size_t)nrows;
    struct cell *ng = calloc(cells, sizeof *ng), *na = calloc(cells, sizeof *na);
    uint8_t *nd = calloc((size_t)nrows, 1), *ns = calloc((size_t)nrows, 1), *nas = calloc((size_t)nrows, 1);
    if (!ng || !na || !nd || !ns || !nas) {
        free(ng); free(na); free(nd); free(ns); free(nas);
        say("term: no memory to resize to %dx%d", ncols, nrows);
        return;
    }
    for (size_t i = 0; i < cells; i++) {
        ng[i] = na[i] = (struct cell){ ' ', DEF_FG, DEF_BG };
    }
    int shift = cy >= nrows ? cy - nrows + 1 : 0, keep = ncols < cols ? ncols : cols;
    for (int r = 0; r < nrows && r + shift < rows; r++) {
        memcpy(&ng[(size_t)r * ncols], &grid[(size_t)(r + shift) * cols], sizeof *ng * (size_t)keep);
        memcpy(&na[(size_t)r * ncols], &alt_grid[(size_t)(r + shift) * cols], sizeof *na * (size_t)keep);
        ns[r] = soft[r + shift];
        nas[r] = alt_soft[r + shift];
    }
    free(grid); free(alt_grid); free(dirty); free(soft); free(alt_soft);
    grid = ng; alt_grid = na; dirty = nd; soft = ns; alt_soft = nas;
    cols = ncols;
    rows = nrows;
    cy = clamp(cy - shift, 0, rows - 1);
    cx = clamp(cx, 0, cols - 1);
    saved_cx = clamp(saved_cx, 0, cols - 1);
    saved_cy = clamp(saved_cy, 0, rows - 1);
    alt_cx = clamp(alt_cx, 0, cols - 1);
    alt_cy = clamp(alt_cy, 0, rows - 1);
    top = 0;
    bot = rows - 1;
    wrap = 0;
    view = 0;
    selection_clear();
    memset(dirty, 1, (size_t)rows);
    struct winsize ws = { .ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows };
    ioctl(pty, TIOCSWINSZ, &ws);
    say("term: resized to %dx%d", cols, rows);
}

static int mouse_tracking;

static void csi(uint8_t c, int pty)
{
    (void)pty;
    int p0 = nparam > 0 ? param[0] : 0, p1 = nparam > 1 ? param[1] : 0;
    int n = p0 > 0 ? p0 : 1;

    if (csi_priv) {
        if (p0 == 25 && (c == 'h' || c == 'l')) {
            cursor_vis = (c == 'h');
        } else if (p0 == 7 && (c == 'h' || c == 'l')) {
            autowrap = (c == 'h');
        } else if ((p0 == 1000 || p0 == 1002 || p0 == 1003) && (c == 'h' || c == 'l')) {
            mouse_tracking = c == 'h' ? p0 : 0;
        } else if (p0 == 1006 && (c == 'h' || c == 'l')) {
            mouse_sgr = c == 'h';
        } else if (p0 == 2004 && (c == 'h' || c == 'l')) {
            bracketed_paste = c == 'h';
        } else if ((p0 == 1049 || p0 == 47 || p0 == 1047) && (c == 'h' || c == 'l')) {
            if (c == 'h' && !in_alt) {
                selection_clear();
                memcpy(alt_grid, grid, sizeof *grid * (size_t)(cols * rows));
                if (soft && alt_soft) { memcpy(alt_soft, soft, (size_t)rows); memset(soft, 0, (size_t)rows); }
                alt_cx = cx;
                alt_cy = cy;
                in_alt = 1;
                for (int r = 0; r < rows; r++) {
                    clear_span(r, 0, cols);
                }
                cx = cy = 0;
            } else if (c == 'l' && in_alt) {
                selection_clear();
                memcpy(grid, alt_grid, sizeof *grid * (size_t)(cols * rows));
                if (soft && alt_soft) memcpy(soft, alt_soft, (size_t)rows);
                for (int r = 0; r < rows; r++) {
                    dirty[r] = 1;
                }
                cx = alt_cx;
                cy = alt_cy;
                in_alt = 0;
            }
        }
        return;
    }
    wrap = 0;
    switch (c) {
    case 'A':
        cy = clamp(cy - n, 0, rows - 1);
        break;
    case 'B':
        cy = clamp(cy + n, 0, rows - 1);
        break;
    case 'C':
        cx = clamp(cx + n, 0, cols - 1);
        break;
    case 'D':
        cx = clamp(cx - n, 0, cols - 1);
        break;
    case 'E':
        cx = 0;
        cy = clamp(cy + n, 0, rows - 1);
        break;
    case 'F':
        cx = 0;
        cy = clamp(cy - n, 0, rows - 1);
        break;
    case 'G':
    case '`':
        cx = clamp(n - 1, 0, cols - 1);
        break;
    case 'd':
        cy = clamp(n - 1, 0, rows - 1);
        break;
    case 'H':
    case 'f':
        cy = clamp((p0 > 0 ? p0 : 1) - 1, 0, rows - 1);
        cx = clamp((p1 > 0 ? p1 : 1) - 1, 0, cols - 1);
        break;
    case 'J':
        if (p0 == 0) {
            clear_span(cy, cx, cols);
            for (int r = cy + 1; r < rows; r++) {
                clear_span(r, 0, cols);
            }
        } else if (p0 == 1) {
            for (int r = 0; r < cy; r++) {
                clear_span(r, 0, cols);
            }
            clear_span(cy, 0, cx + 1);
        } else if (p0 == 2 || p0 == 3) {
            for (int r = 0; r < rows; r++) {
                clear_span(r, 0, cols);
            }
        }
        break;
    case 'K':
        if (p0 == 0) {
            clear_span(cy, cx, cols);
        } else if (p0 == 1) {
            clear_span(cy, 0, cx + 1);
        } else if (p0 == 2) {
            clear_span(cy, 0, cols);
        }
        break;
    case 'L':
        if (cy >= top && cy <= bot) {
            scroll_down(cy, bot, n);
        }
        break;
    case 'M':
        if (cy >= top && cy <= bot) {
            scroll_up(cy, bot, n);
        }
        break;
    case '@':
        if (n > cols - cx) {
            n = cols - cx;
        }
        memmove(&grid[cy * cols + cx + n], &grid[cy * cols + cx],
                sizeof *grid * (size_t)(cols - cx - n));
        clear_span(cy, cx, cx + n);
        break;
    case 'P':
        if (n > cols - cx) {
            n = cols - cx;
        }
        memmove(&grid[cy * cols + cx], &grid[cy * cols + cx + n],
                sizeof *grid * (size_t)(cols - cx - n));
        clear_span(cy, cols - n, cols);
        break;
    case 'X':
        clear_span(cy, cx, cx + n);
        break;
    case 'S':
        scroll_up(top, bot, n);
        break;
    case 'T':
        scroll_down(top, bot, n);
        break;
    case 'r':
        top = clamp((p0 > 0 ? p0 : 1) - 1, 0, rows - 1);
        bot = clamp((p1 > 0 ? p1 : rows) - 1, top, rows - 1);
        cx = cy = 0;
        break;
    case 's':
        saved_cx = cx;
        saved_cy = cy;
        break;
    case 'u':
        cx = clamp(saved_cx, 0, cols - 1);
        cy = clamp(saved_cy, 0, rows - 1);
        break;
    case 'm':
        sgr();
        break;
    case 'n':
        if (p0 == 6) {
            char rep[32];
            int len = snprintf(rep, sizeof rep, "\033[%d;%dR", cy + 1, cx + 1);
            queue_output(rep, (size_t)len);
        }
        break;
    case 'c':
        queue_output("\033[?6c", 5);
        break;
    case 'q':
        if (csi_priv == ' ') {
            queue_output("\033[1;0;0c", 8);
        }
        break;
    }
}

static void feed(uint8_t b, int pty)
{
    if (state != ST_NORM && b == 0x1B) {
        state = ST_ESC;
        return;
    }
    if (state == ST_OSC) {
        if (osc_n == 0 && b == 'R') {
            state = ST_NORM;
        } else if (osc_n == 0 && b == 'P') {
            state = ST_OSC_P;
        } else if (b == '\a' || b == '\n') {
            state = ST_NORM;
        } else {
            osc_n++;
        }
        return;
    }
    if (state == ST_OSC_P) {
        if (++osc_n >= 7) {
            state = ST_NORM;
        }
        return;
    }
    if (state == ST_CHARSET) {
        state = ST_NORM;
        return;
    }
    if (b < 0x20 || b == 0x7F) {
        if (b == 0x1B) {
            state = ST_ESC;
        } else if (b == '\r') {
            cx = wrap = 0;
        } else if (b == '\n' || b == '\v' || b == '\f') {
            if (soft) soft[cy] = 0;
            newline();
        } else if (b == '\b') {
            wrap = 0;
            if (cx > 0) {
                cx--;
            }
        } else if (b == '\t') {
            wrap = 0;
            cx = (cx + 8) & ~7;
            if (cx >= cols) {
                cx = cols - 1;
            }
        }
        return;
    }
    if (state == ST_ESC) {
        state = ST_NORM;
        if (b == '[') {
            state = ST_CSI;
            csi_priv = nparam = param[0] = 0;
        } else if (b == ']') {
            state = ST_OSC;
            osc_n = 0;
        } else if (b == '(' || b == ')' || b == '#' || b == '%') {
            state = ST_CHARSET;
        } else if (b == 'D') {
            newline();
        } else if (b == 'E') {
            cx = 0;
            newline();
        } else if (b == 'M') {
            wrap = 0;
            if (cy == top) {
                scroll_down(top, bot, 1);
            } else if (cy > 0) {
                cy--;
            }
        } else if (b == '7') {
            saved_cx = cx;
            saved_cy = cy;
            saved_fg = cur_fg;
            saved_bg = cur_bg;
            saved_bold = bold;
            saved_rev = rev;
        } else if (b == '8') {
            cx = clamp(saved_cx, 0, cols - 1);
            cy = clamp(saved_cy, 0, rows - 1);
            cur_fg = saved_fg;
            cur_bg = saved_bg;
            bold = saved_bold;
            rev = saved_rev;
            wrap = 0;
        } else if (b == 'c') {
            reset_term();
        }
        return;
    }
    if (state == ST_CSI) {
        if (b == '?' || b == '>') {
            csi_priv = b;
        } else if (b >= '0' && b <= '9') {
            if (nparam == 0) {
                nparam = 1;
            }
            param[nparam - 1] = param[nparam - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (nparam == 0) {
                nparam = 1;
            }
            if (nparam < MAX_PARAMS) {
                param[nparam++] = 0;
            }
        } else if (b >= 0x40 && b <= 0x7E) {
            state = ST_NORM;
            csi(b, pty);
        }
        return;
    }
    if (utf_need > 0) {
        if ((b & 0xC0) == 0x80) {
            utf_cp = (utf_cp << 6) | (b & 0x3F);
            if (--utf_need == 0) {
                put_cp(utf_cp);
            }
            return;
        }
        utf_need = 0;
    }
    if (b < 0x80) {
        put_cp(b);
    } else if ((b & 0xE0) == 0xC0) {
        utf_cp = b & 0x1F;
        utf_need = 1;
    } else if ((b & 0xF0) == 0xE0) {
        utf_cp = b & 0x0F;
        utf_need = 2;
    } else if ((b & 0xF8) == 0xF0) {
        utf_cp = b & 0x07;
        utf_need = 3;
    } else {
        put_cp('?');
    }
}

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp == 0) {
        return 0;
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10ffff) {
        out[0] = (char)(0xf0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (char)(0x80 | (cp & 0x3f));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static int str_cells(const char *s)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    while (p < end) {
        uint8_t b = *p++;
        int n = (b < 0x80) ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        uint32_t cp = b & (0x3Fu >> n);
        for (int i = 0; i < n && p < end; i++) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        w += is_wide(cp) ? 2 : 1;
    }
    return w;
}

static void show_toast(const char *msg)
{
    snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_expire = uptime() + 0.8; /* Disappear after 0.8s */
    toast_active = 1;

    toast_w = str_cells(toast_msg);
    toast_col = (cols > toast_w) ? (cols - toast_w) / 2 : 0;
    toast_row = (rows > 5) ? rows - 4 : (rows > 1 ? rows - 1 : 0);
    if (toast_row >= 0 && toast_row < rows) {
        dirty[toast_row] = 1;
    }
}

static void hide_toast(void)
{
    if (toast_active) {
        toast_active = 0;
        if (toast_row >= 0 && toast_row < rows) {
            dirty[toast_row] = 1;
        }
    }
}

/* What row r shows: the history when scrolled back, else the screen. */
static const struct cell *shown_row(int r)
{
    int line = hist_len - view + r;
    if (view > 0 && line < hist_len) {
        return &history[(size_t)((hist_head + line) % HISTORY) * hist_cols];
    }
    return &grid[(r - (view > 0 ? view : 0)) * cols];
}

static int shown_cols(int r)
{
    return view > 0 && hist_len - view + r < hist_len ? (hist_cols < cols ? hist_cols : cols) : cols;
}

static int shown_soft(int r)
{
    int line = hist_len - view + r;
    if (view > 0 && line < hist_len) return history_soft[(hist_head + line) % HISTORY];
    return soft[r - (view > 0 ? view : 0)];
}

static void scroll_view(int lines)
{
    int v = view + lines;
    v = v < 0 ? 0 : v > hist_len ? hist_len : v;
    if (v == view) return;
    view = v;
    selection_clear();
    memset(dirty, 1, (size_t)rows);
}

static int had_overlay;

static void draw(void)
{
    char buf[1024], row1[512], row2[512];
    int active = ime_get_overlay(row1, row2, sizeof row1);

    /* The status bar over screen row 0 */
    screen_bar("term", ime_mode_label());

    if (toast_active && uptime() >= toast_expire) {
        hide_toast();
    }

    if (had_overlay && (!active || row2[0] == '\0')) {
        if (rows >= 2) {
            dirty[rows - 2] = 1;
        }
        dirty[rows - 1] = 1;
    }
    had_overlay = active;
    dirty[cy] = 1;
    /* A program that follows the mouse gets the arrow; text, the beam. */
    screen_cursor(mouse_tracking ? CURSOR_ARROW : CURSOR_TEXT);
    for (int r = 0; r < rows; r++) {
        if (!dirty[r]) {
            continue;
        }
        dirty[r] = 0;
        const struct cell *line = shown_row(r);
        int c = 0, n = shown_cols(r);
        if (n < cols) {
            screen_fill(n, r + 1, cols - n, 1, DEF_BG);
        }
        while (c < n) {
            struct cell first = line[c];
            if (first.ch == 0) {
                c++;
                continue;
            }
            int start = c;
            size_t len = 0;
            while (c < n && line[c].fg == first.fg && line[c].bg == first.bg && len + 4 < sizeof buf) {
                if (line[c].ch != 0) {
                    len += put_utf8(buf + len, line[c].ch);
                }
                c++;
            }
            buf[len] = '\0';
            screen_text(start, r + 1, buf, first.fg, first.bg);
        }
    }
    if (selection.active) {
        int a = selection.anchor < selection.end ? selection.anchor : selection.end;
        int b = selection.anchor > selection.end ? selection.anchor : selection.end;
        for (int i = a; i <= b; i++) {
            if (i % cols >= shown_cols(i / cols)) continue;
            struct cell cell = shown_row(i / cols)[i % cols];
            if (!cell.ch) continue;
            buf[put_utf8(buf, cell.ch)] = '\0';
            screen_text(i % cols, i / cols + 1, buf, 0xFFFFFF, 0x305878);
            dirty[i / cols] = 1;
        }
    }
    if (cursor_vis && cy + view < rows) {
        struct cell cur = grid[cy * cols + cx];
        buf[put_utf8(buf, cur.ch ? cur.ch : ' ')] = '\0';
        screen_text(cx, cy + view + 1, buf, cur.bg, cur.fg);
    }
    if (view > 0) {
        char note[64];
        snprintf(note, sizeof note, _(" %d/%d lines back "), view, hist_len);
        screen_text(cols - str_cells(note), 1, note, DEF_BG, 0xB8D8FF);
        dirty[0] = 1;
    }
    if (toast_active) {
        screen_fill(toast_col, toast_row + 1, toast_w, 1, 0x183860);
        screen_text(toast_col, toast_row + 1, toast_msg, 0xFFFFFF, 0x183860);
    }
    if (active) {
        if (row2[0] != '\0' && rows >= 2) {
            screen_fill(0, rows - 1, cols, 1, 0x182848);
            screen_text(1, rows - 1, row1, 0xFFFFFF, 0x182848);
            screen_fill(0, rows, cols, 1, 0x182848);
            screen_text(1, rows, row2, 0xB8D8FF, 0x182848);
        } else {
            screen_fill(0, rows, cols, 1, 0x182848);
            screen_text(1, rows, row1, 0xFFFFFF, 0x182848);
        }
    }
    screen_flush();
}

static char *selection_text(size_t *size)
{
    *size = 0;
    if (!selection.active) return NULL;
    int a = selection.anchor < selection.end ? selection.anchor : selection.end;
    int b = selection.anchor > selection.end ? selection.anchor : selection.end;
    char *text = malloc((size_t)(b - a + rows + 1) * 4 + 1);
    if (!text) return NULL;
    for (int r = a / cols; r <= b / cols; r++) {
        const struct cell *line = shown_row(r);
        int n = shown_cols(r);
        int first = r == a / cols ? a % cols : 0;
        int last = r == b / cols ? b % cols : cols - 1;
        if (last >= n) last = n - 1;
        if (first > 0 && first < n && line[first].ch == 0) first--;
        if (!shown_soft(r)) {
            while (last >= first && line[last].ch == ' ') last--;
        }
        for (int c = first; c <= last; c++) {
            uint32_t cp = line[c].ch;
            if (cp) *size += put_utf8(text + *size, cp);
        }
        if (r < b / cols && !shown_soft(r)) text[(*size)++] = '\n';
    }
    text[*size] = '\0';
    return text;
}

static void copy_selection(int primary)
{
    size_t size;
    char *text = selection_text(&size);
    if (!text) { if (!primary) show_toast(_("No selection")); return; }
    int status = clipboard_set(primary, text, size);
    free(text);
    if (!primary || status != CLIPBOARD_OK) show_toast(status == CLIPBOARD_OK ? _("Copied") : _(clipboard_error(status)));
}

static int queue_paste(const char *text, size_t size)
{
    size_t pending = output.size - output.offset;
    if (pending > OUTPUT_LIMIT - 12 || size > OUTPUT_LIMIT - pending - 12) return -1;
    char *clean = malloc(size + 1);
    if (!clean) return -1;
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0xc2 && i + 1 < size && (unsigned char)text[i + 1] >= 0x80 &&
            (unsigned char)text[i + 1] <= 0x9f) { i++; continue; }
        if (c == '\r') {
            clean[n++] = bracketed_paste ? '\n' : '\r';
            if (i + 1 < size && text[i + 1] == '\n') i++;
        } else if (c == '\n') clean[n++] = bracketed_paste ? '\n' : '\r';
        else if (c == '\t' || (c >= 0x20 && c != 0x7f)) clean[n++] = (char)c;
    }
    size_t total = n + (bracketed_paste ? 12 : 0);
    char *packet = malloc(total ? total : 1);
    if (!packet) { free(clean); return -1; }
    if (bracketed_paste) {
        memcpy(packet, "\033[200~", 6);
        memcpy(packet + 6, clean, n);
        memcpy(packet + 6 + n, "\033[201~", 6);
    } else if (n) memcpy(packet, clean, n);
    int ok = queue_output(packet, total);
    free(packet);
    free(clean);
    return ok;
}

static void paste_clipboard(int primary)
{
    int fd;
    size_t size;
    int status = clipboard_get(primary, &fd, &size);
    if (status != CLIPBOARD_OK) { show_toast(_(clipboard_error(status))); return; }
    if (size > OUTPUT_LIMIT) { close(fd); show_toast(_("Paste exceeds memory budget")); return; }
    char *text = malloc(size + 1);
    if (!text) { close(fd); show_toast(_("Cannot allocate paste")); return; }
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = pread(fd, text + offset, size - offset, (off_t)offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (offset != size || queue_paste(text, size) < 0) show_toast(_("Cannot queue paste"));
    free(text);
}

/* A word: letters, digits and these, or a run of anything else but spaces. */
static int word_class(uint32_t cp)
{
    return cp == ' ' || cp == 0 ? 0 : (cp >= 0x80 || (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' &&
           (cp | 0x20) <= 'z') || strchr("_-./~:@%+", (int)cp)) ? 1 : 2;
}

static void pointer_input(const struct pointer_event *event)
{
    int col = clamp(event->col, 0, cols - 1), row = clamp(event->row - 1, 0, rows - 1); /* row 0: the bar */
    if (mouse_tracking && !(keys_modifiers() & KEYS_SHIFT)) {
        char sequence[64];
        int button = event->button == BTN_LEFT ? 0 : event->button == BTN_MIDDLE ? 1 : 2;
        if (event->wheel) button = event->wheel > 0 ? 64 : 65;
        else if (!event->button) {
            if (mouse_tracking != 1003 && !(mouse_tracking == 1002 && event->buttons)) return;
            button = event->buttons & 1 ? 32 : event->buttons & 4 ? 33 : event->buttons & 2 ? 34 : 35;
        }
        if (keys_modifiers() & KEYS_CTRL) button |= 16;
        if (keys_modifiers() & KEYS_ALT) button |= 8;
        for (int i = 0; i < (event->wheel ? abs(event->wheel) : 1); i++) {
            if (mouse_sgr) {
                int n = snprintf(sequence, sizeof sequence, "\033[<%d;%d;%d%c", button, col + 1, row + 1,
                                 event->button && !event->pressed ? 'm' : 'M');
                queue_output(sequence, (size_t)n);
            } else if (col < 223 && row < 223) {
                char report[] = { 27, '[', 'M', (char)(32 + (event->button && !event->pressed ? 3 : button)),
                                  (char)(33 + col), (char)(33 + row) };
                queue_output(report, sizeof report);
            }
        }
        return;
    }
    if (event->wheel) {
        if (in_alt) { /* a full-screen program: the wheel is the arrow keys */
            for (int i = 0; i < 3 * abs(event->wheel); i++) {
                queue_output(event->wheel > 0 ? "\033[A" : "\033[B", 3);
            }
        } else {
            scroll_view(3 * event->wheel);
        }
        return;
    }
    if (event->button == BTN_MIDDLE && event->pressed) { paste_clipboard(1); return; }
    if (event->button == BTN_LEFT && event->pressed) {
        selection.active = selection.dragging = 1;
        selection.anchor = selection.end = row * cols + col;
        const struct cell *line = shown_row(row);
        int n = shown_cols(row);
        if (event->clicks == 2 && col < n) { /* a word */
            int k = word_class(line[col].ch), a = col, b = col;
            while (a > 0 && word_class(line[a - 1].ch) == k) a--;
            while (b + 1 < n && word_class(line[b + 1].ch) == k) b++;
            selection.anchor = row * cols + a;
            selection.end = row * cols + b;
        } else if (event->clicks == 3) { /* a line */
            selection.anchor = row * cols;
            selection.end = row * cols + n - 1;
        }
        if (event->clicks > 1) {
            selection.dragging = 0;
            copy_selection(1);
        }
    } else if (selection.dragging) {
        int end = row * cols + col;
        if (end == selection.end && event->button != BTN_LEFT) return;
        selection.end = end;
        if (event->button == BTN_LEFT && !event->pressed) {
            selection.dragging = 0;
            copy_selection(1);
        }
    } else {
        return; /* a motion: the screen's cursor moved by itself */
    }
    memset(dirty, 1, (size_t)rows);
}

static void send_key(int pty, int key)
{
    (void)pty;
    const char *s = NULL;
    char ch;

    if ((key == K_PGUP || key == K_PGDN) && keys_modifiers() == KEYS_SHIFT && !in_alt) {
        scroll_view((key == K_PGUP ? 1 : -1) * (rows - 1));
        return;
    }
    scroll_view(-view); /* typing shows the screen again */
    int mods = keys_modifiers();
    if ((mods & KEYS_ALT) && (key < 0x80 || key == K_TEXT ||
                              (key >= K_CTRL_0 && key <= K_CTRL_0 + 9))) queue_output("\033", 1);
    if (key == K_CTRL_0 + 2) { queue_output("\0", 1); return; }
    if (key == K_CTRL_0 + 3) { queue_output("\033", 1); return; }
    if (key == K_CTRL_0 + 4) { queue_output("\034", 1); return; }
    if (key == K_CTRL_0 + 5) { queue_output("\035", 1); return; }
    if (key == K_CTRL_0 + 6) { queue_output("\036", 1); return; }
    if (key == K_CTRL_0 + 7) { queue_output("\037", 1); return; }
    if (key == K_CTRL_0 + 8) { queue_output("\177", 1); return; }
    if ((mods & KEYS_CTRL) && (key == '@' || key == ' ')) { queue_output("\0", 1); return; }
    if ((mods & KEYS_CTRL) && (key == '^' || key == '~')) { queue_output("\036", 1); return; }
    if ((mods & KEYS_CTRL) && key == '?') { queue_output("\177", 1); return; }
    if ((mods & KEYS_CTRL) && key == '\b') { queue_output("\b", 1); return; }
    if (key == '\t' && (mods & KEYS_SHIFT)) { queue_output("\033[Z", 3); return; }
    int modifier = 1 + !!(mods & KEYS_SHIFT) + 2 * !!(mods & KEYS_ALT) + 4 * !!(mods & KEYS_CTRL);
    if (modifier > 1) {
        const char *final = key == K_UP ? "A" : key == K_DOWN ? "B" : key == K_RIGHT ? "C" :
                            key == K_LEFT ? "D" : key == K_BOL ? "H" : key == K_EOL ? "F" : NULL;
        if (final) {
            char sequence[24];
            int n = snprintf(sequence, sizeof sequence, "\033[1;%d%s", modifier, final);
            queue_output(sequence, (size_t)n);
            return;
        }
        int number = key == K_INS ? 2 : key == K_DEL ? 3 : key == K_PGUP ? 5 : key == K_PGDN ? 6 : 0;
        if (key >= K_F1 && key < K_F1 + 12) {
            static const int numbers[] = { 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24 };
            number = numbers[key - K_F1];
            if (key < K_F1 + 4) {
                char sequence[24];
                int n = snprintf(sequence, sizeof sequence, "\033[1;%d%c", modifier, 'P' + key - K_F1);
                queue_output(sequence, (size_t)n);
                return;
            }
        }
        if (number) {
            char sequence[24];
            int n = snprintf(sequence, sizeof sequence, "\033[%d;%d~", number, modifier);
            queue_output(sequence, (size_t)n);
            return;
        }
    }

    switch (key) {
    case '\n':
        s = "\r";
        break;
    case '\b':
        s = "\177";
        break;
    case K_UP:
        s = "\033[A";
        break;
    case K_DOWN:
        s = "\033[B";
        break;
    case K_RIGHT:
        s = "\033[C";
        break;
    case K_LEFT:
        s = "\033[D";
        break;
    case K_BOL:
        s = "\033[1~";
        break;
    case K_DEL:
        s = "\033[3~";
        break;
    case K_EOL:
        s = "\033[4~";
        break;
    case K_PGUP:
        s = "\033[5~";
        break;
    case K_PGDN:
        s = "\033[6~";
        break;
    case K_INS:
        s = "\033[2~";
        break;
    default:
        if (key >= K_F1 && key < K_F1 + 12) { /* as xterm sends them */
            static const char *const fkeys[12] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~",
                "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~" };
            s = fkeys[key - K_F1];
            break;
        }
        if (key > 0 && key < 0x80) {
            ch = (char)key;
            queue_output(&ch, 1);
        } else if (key == K_TEXT) {
            queue_output(keys_text(), strlen(keys_text()));
        }
        return;
    }
    queue_output(s, strlen(s));
}

/* Wi-Fi phys moved into locachy-net leave dangling rfkill symlinks in the
 * host root network namespace, which abort bluetui on startup. Hide only
 * unreadable rfkill entries in a private mount namespace while keeping
 * bluetui in the host network namespace with no network access. */
static void filter_rfkill_for_bluetui(const char *prog)
{
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    if (strcmp(base, "bluetui") != 0) return;
    char names[16][64], targets[16][PATH_MAX];
    int n = 0;
    DIR *d = opendir("/sys/class/rfkill");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 16) {
        if (strncmp(e->d_name, "rfkill", 6) != 0) continue;
        char path[128], type[160];
        snprintf(path, sizeof path, "/sys/class/rfkill/%.63s", e->d_name);
        snprintf(type, sizeof type, "%s/type", path);
        if (access(type, R_OK) == 0 && realpath(path, targets[n]) != NULL) {
            snprintf(names[n], sizeof names[n], "%.63s", e->d_name);
            n++;
        }
    }
    closedir(d);
    if (unshare(CLONE_NEWNS) == 0 && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0 &&
        mount("tmpfs", "/sys/class/rfkill", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=64k") == 0) {
        for (int i = 0; i < n; i++) {
            char dst[128];
            snprintf(dst, sizeof dst, "/sys/class/rfkill/%.63s", names[i]);
            int r = symlink(targets[i], dst);
            (void)r;
        }
    }
}

int main(int argc, char **argv)
{
    static char *default_argv[] = { "/bin/bash", "-l", NULL };
    char **cmd = argc > 1 ? argv + 1 : default_argv;

    screen_open(BASE_FONT);
    keys_open();
    cols = screen_cols();
    rows = screen_rows() - 1;
    if (cols <= 0 || rows <= 0) {
        say("term: no screen");
        return 1;
    }
    grid = calloc((size_t)(cols * rows), sizeof *grid);
    alt_grid = calloc((size_t)(cols * rows), sizeof *alt_grid);
    dirty = calloc((size_t)rows, 1);
    soft = calloc((size_t)rows, 1);
    alt_soft = calloc((size_t)rows, 1);
    if (!grid || !alt_grid || !dirty || !soft || !alt_soft) return 1;
    reset_term();

    int pty = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char *pts = pty >= 0 && grantpt(pty) == 0 && unlockpt(pty) == 0 ? ptsname(pty) : NULL;
    int s = pts != NULL ? open(pts, O_RDWR | O_NOCTTY) : -1;
    if (s < 0) {
        say("term: cannot open a pty: %s", strerror(errno));
        return 1;
    }
    struct winsize ws = { .ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows };
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        ioctl(s, TIOCSCTTY, 0);
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) {
            close(s);
        }
        setenv("TERM", "xterm-256color", 1); /* what this terminal is; the rest as process 1 set it */
        filter_rfkill_for_bluetui(cmd[0]);
        execvp(cmd[0], cmd);
        dprintf(2, "term: %s: %s\n", cmd[0], strerror(errno));
        _exit(127);
    }
    close(s);
    fcntl(pty, F_SETFL, O_NONBLOCK);
    say("term: started %s (pid %d), %dx%d", cmd[0], (int)pid, cols, rows);
    ime_init(NULL, NULL);

    int shown = 0;
    for (;;) {
        int prev_cy = cy;
        int timeout = toast_active ? 40 : 2000;
        struct pollfd p = { .fd = pty, .events = POLLIN | (output.size > output.offset ? POLLOUT : 0) };
        int key = key_poll_many(&p, 1, timeout);
        if (p.revents & POLLOUT) flush_output(pty);
        dirty[prev_cy] = 1;
        const char *ime_notice = NULL;
        if (key > 0 && key != K_CTRL_SPACE && key != K_ACT_IME_CN && key != K_ACT_IME_RAW &&
            key != K_ACT_IME_WIDTH && key != K_POINTER && toast_active) {
            hide_toast();
        }
        if (key == K_POINTER) {
            pointer_input(pointer_last());
        } else if (key == K_SHOWN) {
            say("term: shown %d (pid %d)", ++shown, (int)getpid());
            resize(pty); /* the scale may have changed meanwhile */
            for (int r = 0; r < rows; r++) {
                dirty[r] = 1;
            }
        } else if (ime_action(key, &ime_notice)) {
            if (ime_notice) {
                show_toast(ime_notice);
            }
            say("term: ime mode %s", ime_mode_label());
        } else if (ime_mode() == IME_MODE_CN && ime_width_key(key, keys_modifiers())) {
            const char *msg = ime_toggle_width(); /* Chinese mode's width */
            if (msg) {
                show_toast(msg);
            }
        } else if (key == 3 && keys_modifiers() == (KEYS_CTRL | KEYS_SHIFT)) {
            copy_selection(0);
        } else if (key == 22 && keys_modifiers() == (KEYS_CTRL | KEYS_SHIFT)) {
            paste_clipboard(0);
        } else if (key > 0) {
            int taken = ime_process_key(key);
            const char *commit = ime_get_commit(); /* before the key, if it passes on */
            if (commit != NULL && commit[0] != '\0') {
                queue_output(commit, strlen(commit));
            }
            if (!taken) {
                send_key(pty, key);
            }
        }
        flush_output(pty);
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = read(pty, buf, sizeof buf);
            if (n > 0) {
                if (view > 0) scroll_view(-view); /* new output shows the screen */
                for (ssize_t i = 0; i < n; i++) {
                    feed(buf[i], pty);
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                break;
            }
            int status = 0;
            waitpid(pid, &status, 0);
            status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            say("term: %s (pid %d) exited with status %d", cmd[0], (int)pid, status);
            ime_done();
            return status;
        }
        draw();
    }
}
