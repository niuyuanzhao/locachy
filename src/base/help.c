/* F1's help panel (base.h ui_help): the program's own keys, then the keys every
 * program has. The table is key-action.c's, so a program that shows this links it. */
#include <stdlib.h>
#include <string.h>

#include "base.h"

/* The colours of ui.c's menus: the panel looks like them. */
#define MENU_FG 0xD0D0D0
#define MENU_BG 0x203048
#define MENU_ON 0x80C0FF

/* Weak, as in settings.c: a test that links this without keys.c reads no
 * modifiers and no pointer. One strong reference here would make the symbol
 * strong for every object it is linked with, also where settings.c asks
 * whether it is there. */
__attribute__((weak)) int keys_modifiers(void);
__attribute__((weak)) const struct pointer_event *pointer_last(void);

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
    for (;;) {
        int cols = screen_cols(), rows = screen_rows(), body = rows > 2 ? rows - 2 : 1;
        if (top > total - body) top = total - body;
        if (top < 0) top = 0;
        screen_fill(0, 0, cols, rows, MENU_BG);
        screen_text(1, 0, title, MENU_ON, MENU_BG);
        for (int r = 0; r < body && top + r < total; r++) {
            int i = top + r;
            const struct ui_help_line *l = NULL;
            if (i < own) {
                l = &lines[i];
            } else if (i == own + gap) {
                screen_text(1, r + 1, _("Every program"), MENU_ON, MENU_BG);
            } else if (i > own + gap) {
                l = &every_program[i - own - gap - 1];
            }
            if (l) {
                char pad[256];
                screen_text(2, r + 1, ui_pad(pad, sizeof pad, l->keys, keyw), MENU_FG, MENU_BG);
                screen_text(4 + keyw, r + 1, _(l->what), MENU_FG, MENU_BG);
            }
        }
        screen_text(1, rows - 1, _("Esc, F1 or q: close"), MENU_ON, MENU_BG);
        screen_flush();
        int key = key_poll_timeout(-1, -1);
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
        } else if (key == K_POINTER) {
            if (pointer_last) top -= 3 * pointer_last()->wheel;
        } else {
            int act = key_list_action(key, mods);
            if (act == ACT_QUIT) return ACT_QUIT;
            if (act == ACT_CANCEL || act == ACT_HELP) return ACT_NONE;
        }
    }
}
