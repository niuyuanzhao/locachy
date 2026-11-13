/* key_action: the one table of what keys mean (base/key-action.c). Each
 * case is a key as keys.c delivers it (Ctrl with a letter: its control
 * character, Ctrl with a digit: K_CTRL_0 + n) with the modifiers reported
 * beside it, and the action every program gets for it. */
#include <stdio.h>

#include "base.h"

static int failed;

static void check(int got, int want, const char *what)
{
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s: action %d, want %d\n", what, got, want);
        failed = 1;
    }
}

#define C(k) ((k) & 0x1F)

int main(void)
{
    /* Quit */
    check(key_action(C('q'), KEYS_CTRL), ACT_QUIT, "Ctrl+Q quits: it comes as 17, not 'q'");
    check(key_action('q', KEYS_CTRL), ACT_NONE, "'q' with Ctrl is not what the key layer gives: no action");
    check(key_action('q', 0), ACT_NONE, "q alone is no action of the table");
    check(key_action(K_CLOSE, 0), ACT_QUIT, "the window's close asks to quit");
    check(key_action(K_F1 + 3, KEYS_ALT), ACT_QUIT, "Alt+F4 asks to quit");
    /* Esc, home, help */
    check(key_action(27, 0), ACT_CANCEL, "Esc cancels");
    check(key_action(27, KEYS_SHIFT), ACT_CANCEL, "Shift+Esc cancels");
    check(key_action(27, KEYS_CTRL), ACT_CANCEL, "Ctrl+[ has Esc's code: cancels");
    check(key_action(27, KEYS_META), ACT_NONE, "Super+Esc is process 1's home: no program action");
    check(key_action(K_ACT_HOME, 0), ACT_NONE, "K_ACT_HOME is process 1's: no program action");
    check(key_action(K_F1, 0), ACT_HELP, "F1 is help");
    /* List: q is Esc, never quit */
    check(key_list_action('q', 0), ACT_CANCEL, "in a list q is Esc");
    check(key_list_action('q', KEYS_ALT), ACT_NONE, "Alt+q is not q");
    check(key_list_action('q', KEYS_META), ACT_NONE, "Super+q is not q");
    check(key_list_action('Q', KEYS_SHIFT), ACT_NONE, "Shift+q is not q");
    check(key_list_action(C('q'), KEYS_CTRL), ACT_QUIT, "in a list Ctrl+Q still quits");
    /* Navigation and focus */
    check(key_action(K_LEFT, KEYS_ALT), ACT_BACK, "Alt+Left is back");
    check(key_action(K_RIGHT, KEYS_ALT), ACT_FORWARD, "Alt+Right is forward");
    check(key_action(K_UP, KEYS_ALT), ACT_UP, "Alt+Up is the parent");
    check(key_action('\t', 0), ACT_NEXT_FIELD, "Tab is the next field");
    check(key_action('\t', KEYS_SHIFT), ACT_PREV_FIELD, "Shift+Tab is the previous field");
    /* Clipboard and editing */
    check(key_action(C('c'), KEYS_CTRL), ACT_COPY, "Ctrl+C copies");
    check(key_action(C('c'), KEYS_CTRL | KEYS_SHIFT), ACT_COPY, "Ctrl+Shift+C copies");
    check(key_action(K_INS, KEYS_CTRL), ACT_COPY, "Ctrl+Insert copies");
    check(key_action(K_ACT_COPY, 0), ACT_COPY, "a Ctrl+C button copies (a click has no modifiers)");
    check(key_action(K_ACT_SELECT_ALL, 0), ACT_SELECT_ALL, "a Ctrl+A button selects all");
    check(key_action('y', 0), ACT_NONE, "y is not copy");
    check(key_list_action('y', 0), ACT_NONE, "nor in a list");
    check(key_action(C('x'), KEYS_CTRL), ACT_CUT, "Ctrl+X cuts");
    check(key_action(C('v'), KEYS_CTRL), ACT_PASTE, "Ctrl+V pastes");
    check(key_action(K_INS, KEYS_SHIFT), ACT_PASTE, "Shift+Insert pastes");
    check(key_action(C('z'), KEYS_CTRL), ACT_UNDO, "Ctrl+Z undoes");
    check(key_action(C('z'), KEYS_CTRL | KEYS_SHIFT), ACT_REDO, "Ctrl+Shift+Z redoes");
    check(key_action(C('a'), KEYS_CTRL), ACT_SELECT_ALL, "Ctrl+A selects all");
    check(key_action(C('a'), KEYS_CTRL | KEYS_SHIFT), ACT_DESELECT, "Ctrl+Shift+A deselects");
    /* Find */
    check(key_action(C('f'), KEYS_CTRL), ACT_FIND, "Ctrl+F finds");
    check(key_action(C('f'), KEYS_CTRL | KEYS_SHIFT), ACT_FIND_IN_FILES, "Ctrl+Shift+F finds in files");
    check(key_action(K_F1 + 2, 0), ACT_FIND_NEXT, "F3 is the next match");
    check(key_action(K_F1 + 2, KEYS_SHIFT), ACT_FIND_PREV, "Shift+F3 is the previous match");
    /* Documents and tabs */
    check(key_action(C('n'), KEYS_CTRL), ACT_NEW, "Ctrl+N is new");
    check(key_action(C('n'), KEYS_CTRL | KEYS_SHIFT), ACT_NEW_FOLDER, "Ctrl+Shift+N is a new folder");
    check(key_action(C('o'), KEYS_CTRL), ACT_OPEN, "Ctrl+O opens");
    check(key_action(C('s'), KEYS_CTRL), ACT_SAVE, "Ctrl+S saves");
    check(key_action(C('s'), KEYS_CTRL | KEYS_SHIFT), ACT_SAVE_AS, "Ctrl+Shift+S saves as");
    check(key_action(C('t'), KEYS_CTRL), ACT_NEW_TAB, "Ctrl+T is a new tab");
    check(key_action(C('t'), KEYS_CTRL | KEYS_SHIFT), ACT_REOPEN_TAB, "Ctrl+Shift+T reopens a tab");
    check(key_action(C('w'), KEYS_CTRL), ACT_CLOSE_TAB, "Ctrl+W closes the tab");
    check(key_action(K_PGDN, KEYS_CTRL), ACT_NEXT_TAB, "Ctrl+PgDn is the next tab");
    check(key_action(K_PGUP, KEYS_CTRL), ACT_PREV_TAB, "Ctrl+PgUp is the previous tab");
    check(key_action('\t', KEYS_CTRL), ACT_NEXT_TAB, "Ctrl+Tab is the next tab");
    check(key_action('\t', KEYS_CTRL | KEYS_SHIFT), ACT_PREV_TAB, "Ctrl+Shift+Tab is the previous tab");
    /* Items */
    check(key_action(K_F1 + 1, 0), ACT_RENAME, "F2 renames");
    check(key_action(K_DEL, 0), ACT_DELETE, "Delete deletes");
    check(key_action(K_DEL, KEYS_SHIFT), ACT_DELETE_PERMANENT, "Shift+Delete deletes for good");
    check(key_action('\n', KEYS_ALT), ACT_PROPERTIES, "Alt+Enter shows properties");
    check(key_action(K_F1 + 4, 0), ACT_RELOAD, "F5 reloads");
    check(key_action('.', KEYS_ALT), ACT_HIDDEN, "Alt+. shows hidden files");
    check(key_action(C('h'), KEYS_CTRL), ACT_NONE, "Ctrl+H is Backspace's code: no action");
    /* Zoom and menus */
    check(key_action('=', KEYS_CTRL), ACT_ZOOM_IN, "Ctrl+= zooms in");
    check(key_action('+', KEYS_CTRL | KEYS_SHIFT), ACT_ZOOM_IN, "Ctrl++ zooms in");
    check(key_action('-', KEYS_CTRL), ACT_ZOOM_OUT, "Ctrl+- zooms out");
    check(key_action(K_CTRL_0, KEYS_CTRL), ACT_ZOOM_RESET, "Ctrl+0 is the actual size");
    check(key_action(K_F1 + 9, KEYS_SHIFT), ACT_CONTEXT_MENU, "Shift+F10 opens the context menu");
    check(key_action(K_F1 + 9, 0), ACT_NONE, "F10 alone is no action");
    /* Not actions */
    check(key_action('c', KEYS_META), ACT_NONE, "Super+letter is the desktop's");
    check(key_action(C('c'), KEYS_CTRL | KEYS_ALT), ACT_NONE, "Ctrl+Alt+letter is no action");
    check(key_action('e', KEYS_ALTGR), ACT_NONE, "AltGr types");
    puts(failed ? "FAIL key actions" : "PASS key actions: one table for every program");
    return failed;
}
