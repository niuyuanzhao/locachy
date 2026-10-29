/* What the pointer can click in the framebuffer programs: the places a
 * program registers as it draws, buttons, and a menu over the picture. */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

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
    int n = 0, w = 0, sel = 0, armed = 0; /* the release of the opening click chooses nothing */
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
    if (col + w > cols) col = cols - w;
    if (row + n > rows) row = rows - n;
    col = col < 0 ? 0 : col;
    row = row < 0 ? 0 : row;
    for (;;) {
        for (int i = 0; i < n; i++) {
            uint32_t bg = i == sel ? MENU_ON : MENU_BG, fg = i == sel ? MENU_BG : MENU_FG;
            screen_fill(col, row + i, w, 1, bg);
            screen_text(col + 1, row + i, items[i], fg, bg);
        }
        screen_flush();
        int key = key_poll_timeout(-1, -1);
        if (key == K_UP) {
            sel = (sel + n - 1) % n;
        } else if (key == K_DOWN) {
            sel = (sel + 1) % n;
        } else if (key == '\n') {
            return sel;
        } else if (key == 27 || key == K_SHOWN) {
            return -1;
        } else if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            int inside = e->output == output && e->col >= col && e->col < col + w &&
                         e->row >= row && e->row < row + n;
            if (!e->button && !e->wheel) {
                armed = 1;
                if (inside) sel = e->row - row; /* follows the pointer */
            } else if (e->button == BTN_LEFT || e->button == BTN_RIGHT) {
                if (!inside && e->pressed) return -1;
                if (inside && e->pressed) armed = 1;
                if (inside && !e->pressed && armed) return e->row - row; /* on release, as menus do */
            }
        }
    }
}
