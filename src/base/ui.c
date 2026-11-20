/* What the pointer can click in the framebuffer programs: the places a
 * program registers as it draws, buttons, and a menu over the picture. */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

__attribute__((weak)) void screen_cursor(int shape);
__attribute__((weak)) void key_close_kept(void);

#define BUTTON_FG 0xD0D0D0
#define BUTTON_BG 0x304860
#define MENU_FG   0xD0D0D0
#define MENU_BG   0x203048
#define MENU_ON   0x80C0FF

struct hit {
    int col, row, ncols, nrows, id, output;
};
static struct hit *hits;
static int nhits, hits_size;

void ui_hits_clear(void)
{
    int output = screen_is_extended && screen_is_extended() && screen_output_current ? screen_output_current() : -1;
    if (output < 0) { nhits = 0; return; }
    for (int i = nhits - 1; i >= 0; i--) {
        if (hits[i].output != output) continue;
        memmove(hits + i, hits + i + 1, (size_t)(nhits - i - 1) * sizeof *hits);
        nhits--;
    }
}

void ui_hit(int col, int row, int ncols, int nrows, int id)
{
    if (ncols <= 0 || nrows <= 0) return;
    if (nhits == hits_size) {
        int size = hits_size ? hits_size * 2 : 64;
        struct hit *h = realloc(hits, (size_t)size * sizeof *h);
        if (!h) return;
        hits = h;
        hits_size = size;
    }
    int output = screen_is_extended && screen_is_extended() && screen_output_current ? screen_output_current() : -1;
    hits[nhits++] = (struct hit){ col, row, ncols, nrows, id, output };
}

/* The last added wins: it was drawn over the others. */
int ui_hit_at(int col, int row)
{
    int output = screen_is_extended && screen_is_extended() && screen_output_current ? screen_output_current() : -1;
    for (int i = nhits - 1; i >= 0; i--) {
        const struct hit *h = &hits[i];
        if (h->output != output) continue;
        if (col >= h->col && col < h->col + h->ncols && row >= h->row && row < h->row + h->nrows) {
            return h->id;
        }
    }
    return -1;
}

/* The cells text takes on the screen: a CJK character two, any other one. */
int ui_cells(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char b = *p++;
        int more = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        uint32_t cp = b & (0x3Fu >> more);
        for (; more > 0 && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
        w += screen_char_cells(cp);
    }
    return w;
}

/* s and spaces to cells wide (as %-*s counts bytes, this counts cells), in
 * out; s whole if it is wider. */
const char *ui_pad(char *out, size_t size, const char *s, int cells)
{
    int n = snprintf(out, size, "%s", s);
    for (int w = ui_cells(s); w < cells && n >= 0 && (size_t)n + 1 < size; w++) {
        out[n++] = ' ';
        out[n] = '\0';
    }
    return out;
}

int ui_button(int col, int row, const char *label, int id)
{
    int n = ui_cells(label) + 2;
    if (col + n > screen_cols()) return col;
    screen_fill(col, row, n, 1, BUTTON_BG);
    screen_text(col, row, "[", BUTTON_FG, BUTTON_BG);
    screen_text(col + 1, row, label, BUTTON_FG, BUTTON_BG);
    screen_text(col + n - 1, row, "]", BUTTON_FG, BUTTON_BG);
    ui_hit(col, row, n, 1, id);
    return col + n + 1;
}

int ui_menu(int col, int row, const char *const *items)
{
    int n = 0, w = 0, sel = 0, top = 0, armed = 0;
    const struct pointer_event *opening = pointer_last();
    int press_row = opening && opening->pressed ? opening->row : row;
    while (items[n]) {
        int len = ui_cells(items[n]);
        w = len > w ? len : w;
        n++;
    }
    if (n == 0) return -1;
    w += 2;
    /* Drawn on the selected output: a press on another is outside. */
    int output = screen_is_extended && screen_is_extended() && screen_output_current ? screen_output_current() : 0;
    int cols = screen_cols(), rows = screen_rows();
    if (cols <= 0 || rows <= 0) return -1;
    int min_row = rows > 1, max_rows = rows - min_row;
    int visible = n < max_rows ? n : max_rows;
    if (w > cols) w = cols;
    if (col > cols - w) col = cols - w;
    if (row > rows - visible) row = rows - visible;
    if (col < 0) col = 0;
    if (row < min_row) row = min_row;
    if (screen_cursor) screen_cursor(CURSOR_ARROW);
    int drawn_top = -1, drawn_sel = -1;
    for (;;) {
        if (drawn_top != top || drawn_sel != sel) {
            for (int i = 0; i < visible; i++) {
                int selected = top + i == sel;
                uint32_t bg = selected ? MENU_ON : MENU_BG, fg = selected ? MENU_BG : MENU_FG;
                screen_fill(col, row + i, w, 1, bg);
                screen_text(col + 1, row + i, items[top + i], fg, bg);
            }
            screen_flush();
            drawn_top = top;
            drawn_sel = sel;
        }
        int key = key_poll_timeout(-1, -1);
        if (key == K_UP || key == K_DOWN) {
            sel = (sel + (key == K_UP ? n - 1 : 1)) % n;
            if (sel < top) top = sel;
            if (sel >= top + visible) top = sel - visible + 1;
        } else if (key == '\n') {
            return sel;
        } else if (key == 27 || key == K_SHOWN || key == K_CLOSE) {
            if (key == K_CLOSE && key_close_kept) key_close_kept();
            return -1;
        } else if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (!e) continue;
            if (e->cancelled) { armed = 0; continue; }
            int inside = e->output == output && e->col >= col && e->col < col + w &&
                         e->row >= row && e->row < row + visible;
            if (inside && e->wheel && visible < n) {
                int64_t next = (int64_t)top - e->wheel;
                top = next < 0 ? 0 : next > n - visible ? n - visible : (int)next;
                sel = top + e->row - row;
            } else if (!e->button && !e->wheel && !e->hwheel && !e->gesture &&
                       !e->scroll_dx && !e->scroll_dy) {
                /* The opening release alone must not choose an item. */
                if (!e->buttons || e->row != press_row) armed = 1;
                if (inside) sel = top + e->row - row;
            } else if (e->button == BTN_LEFT || e->button == BTN_RIGHT || e->button == BTN_MIDDLE) {
                if (!inside && e->pressed) return -1;
                if (e->button == BTN_MIDDLE) continue;
                if (inside && !e->pressed && armed) return top + e->row - row;
                armed = e->pressed && inside;
                if (armed) sel = top + e->row - row;
            }
        }
    }
}
