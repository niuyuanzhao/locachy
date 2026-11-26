/* F1's help panel (base.h ui_help): the program's own keys, then the keys every
 * program has. The table is key-action.c's, so a program that shows this links it. */
#include <errno.h>
#include <linux/input-event-codes.h>

#include "base.h"

/* Weak, as in settings.c: a test that links this without keys.c reads no
 * modifiers and no pointer. One strong reference here would make the symbol
 * strong for every object it is linked with, also where settings.c asks
 * whether it is there. */
__attribute__((weak)) int keys_modifiers(void);
__attribute__((weak)) const struct pointer_event *pointer_last(void);
__attribute__((weak)) void screen_cursor(int shape);

static const struct ui_help_line every_program[] = {
    { "Ctrl+Q", N_("Quit, asking first when work would be lost") },
    { "Esc", N_("Close the innermost menu, dialog or search; else back one level") },
    { "Super+Esc", N_("Home; the program waits where it is") },
    { "F1", N_("This help") },
    { "Tab, Shift+Tab", N_("Next or previous field") },
    { "Ctrl+C, Ctrl+X, Ctrl+V", N_("Copy, cut, paste") },
    { "Ctrl+Z, Ctrl+Shift+Z", N_("Undo, redo") },
    { "Ctrl+A", N_("Select all") },
    { "Ctrl+F, F3, Shift+F3", N_("Find; next, previous match") },
    { NULL, NULL },
};

static int help_count(const struct ui_help_line *l, int *keyw)
{
    int n = 0;
    for (; l && l->keys; l++, n++) {
        int w = ui_cells(l->keys);
        if (w > *keyw) *keyw = w;
    }
    return n;
}

int ui_help(const char *title, const struct ui_help_line *lines)
{
    int keyw = 0, own = help_count(lines, &keyw), all = help_count(every_program, &keyw);
    int gap = own ? 1 : 0, total = own + gap + 1 + all, top = 0;
    int dirty = 1, drawn_top = -1, drawn_cols = 0, drawn_rows = 0;
    if (screen_cursor) screen_cursor(CURSOR_ARROW);
    for (;;) {
        int cols = screen_cols(), rows = screen_rows(), body = rows > 3 ? rows - 3 : 0;
        int close_col = cols > 1 ? 1 : 0;
        const char *close_label = _("[Esc / F1: close]");
        int close_w = ui_cells(close_label);
        if (close_w > cols - close_col) close_w = cols - close_col;
        if (top > total - body) top = total - body;
        if (top < 0) top = 0;
        if (dirty || top != drawn_top || cols != drawn_cols || rows != drawn_rows) {
            if (cols > 0 && rows > 1) {
                uint32_t bg = theme_rgb(THEME_PANEL, 0x203048), fg = theme_rgb(THEME_FG, 0xD0D0D0);
                uint32_t heading = theme_rgb(THEME_ACCENT_TEXT, 0x80C0FF);
                screen_fill(0, 1, cols, rows - 1, bg);
                if (rows > 2) screen_text(1, 1, title, heading, bg);
                for (int r = 0; r < body && top + r < total; r++) {
                    int i = top + r;
                    const struct ui_help_line *l = NULL;
                    if (i < own) {
                        l = &lines[i];
                    } else if (i == own + gap) {
                        screen_text(1, r + 2, _("Every program"), heading, bg);
                    } else if (i > own + gap) {
                        l = &every_program[i - own - gap - 1];
                    }
                    if (l) {
                        char pad[256];
                        screen_text(2, r + 2, ui_pad(pad, sizeof pad, l->keys, keyw), fg, bg);
                        screen_text(4 + keyw, r + 2, _(l->what), fg, bg);
                    }
                }
                uint32_t on = theme_rgb(THEME_ON_ACCENT, 0x203048), accent = theme_rgb(THEME_ACCENT, 0x80C0FF);
                screen_text(close_col, rows - 1, close_label, on, accent);
                screen_flush();
            }
            drawn_top = top;
            drawn_cols = cols;
            drawn_rows = rows;
            dirty = 0;
        }
        errno = 0;
        int key = key_poll_timeout(-1, -1);
        if (key == 0 && errno == EINTR) return ACT_NONE;
        int mods = keys_modifiers ? keys_modifiers() : 0;
        if (key == K_UP) {
            top--;
        } else if (key == K_DOWN) {
            top++;
        } else if (key == K_PGUP) {
            top -= body;
        } else if (key == K_PGDN) {
            top += body;
        } else if (key == K_BOL) {
            top = 0;
        } else if (key == K_EOL) {
            top = total;
        } else if (key == K_SHOWN || key == K_REPAINT) {
            dirty = 1;
        } else if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last ? pointer_last() : NULL;
            if (!e) continue;
            int output = screen_is_extended && screen_is_extended() && screen_output_current ? screen_output_current() : 0;
            int inside = e->output == output && e->col >= 0 && e->col < cols && e->row >= 1 && e->row < rows;
            if (e->pressed && !inside) return ACT_NONE;
            if (!inside) continue;
            if (e->button == BTN_LEFT && e->pressed && e->row == rows - 1 &&
                e->col >= close_col && e->col < close_col + close_w) return ACT_NONE;
            int64_t next = (int64_t)top - 3LL * e->wheel;
            top = next < 0 ? 0 : next > total ? total : (int)next;
        } else {
            int act = key_list_action(key, mods);
            if (act == ACT_QUIT) return ACT_QUIT;
            if (act == ACT_CANCEL || act == ACT_HELP) return ACT_NONE;
        }
    }
}
