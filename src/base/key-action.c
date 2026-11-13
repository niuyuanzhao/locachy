/* What keys mean: one table for every program (base.h, KEYS.md). The key
 * is keys.c's value and mods the modifiers it reports beside it: Ctrl with
 * a letter comes as the control character (letter & 0x1F, with or without
 * Shift), Ctrl with a digit as K_CTRL_0 + n, the function keys as K_F1 + n.
 * Super combinations are the desktop's and AltGr types, so neither is an
 * action here. */
#include "base.h"

#define CTRL_OF(c) ((c) & 0x1F)

int key_action(int key, int mods)
{
    int ctrl = mods & KEYS_CTRL, alt = mods & KEYS_ALT, shift = mods & KEYS_SHIFT;
    if (key == K_CLOSE) {
        return ACT_QUIT; /* the window's close asks what Ctrl+Q asks */
    }
    if (key == K_ACT_COPY) {
        return ACT_COPY; /* a button for Ctrl+C */
    }
    if (key == K_ACT_SELECT_ALL) {
        return ACT_SELECT_ALL; /* a button for Ctrl+A */
    }
    if (mods & (KEYS_META | KEYS_ALTGR)) {
        return ACT_NONE;
    }
    if (key == 27 && !alt) {
        return ACT_CANCEL; /* Esc; Ctrl+[ has the same code */
    }
    if (alt && !ctrl) {
        switch (key) {
        case K_LEFT: return ACT_BACK;
        case K_RIGHT: return ACT_FORWARD;
        case K_UP: return ACT_UP;
        case '\n': return ACT_PROPERTIES;
        case '.': return ACT_HIDDEN;
        case K_F1 + 3: return ACT_QUIT; /* Alt+F4 */
        }
        return ACT_NONE;
    }
    if (ctrl && !alt) {
        switch (key) {
        case CTRL_OF('a'): return shift ? ACT_DESELECT : ACT_SELECT_ALL;
        case CTRL_OF('c'): return ACT_COPY;
        case CTRL_OF('f'): return shift ? ACT_FIND_IN_FILES : ACT_FIND;
        case CTRL_OF('n'): return shift ? ACT_NEW_FOLDER : ACT_NEW;
        case CTRL_OF('o'): return shift ? ACT_NONE : ACT_OPEN;
        case CTRL_OF('q'): return ACT_QUIT;
        case CTRL_OF('s'): return shift ? ACT_SAVE_AS : ACT_SAVE;
        case CTRL_OF('t'): return shift ? ACT_REOPEN_TAB : ACT_NEW_TAB;
        case CTRL_OF('v'): return ACT_PASTE;
        case CTRL_OF('w'): return shift ? ACT_NONE : ACT_CLOSE_TAB;
        case CTRL_OF('x'): return shift ? ACT_NONE : ACT_CUT;
        case CTRL_OF('z'): return shift ? ACT_REDO : ACT_UNDO;
        case K_PGDN: return ACT_NEXT_TAB;
        case K_PGUP: return ACT_PREV_TAB;
        case '\t': return shift ? ACT_PREV_TAB : ACT_NEXT_TAB; /* Ctrl+I has Tab's code too */
        case K_INS: return shift ? ACT_NONE : ACT_COPY;
        case '=':
        case '+': return ACT_ZOOM_IN;
        case '-': return ACT_ZOOM_OUT;
        case K_CTRL_0: return ACT_ZOOM_RESET;
        }
        return ACT_NONE;
    }
    if (ctrl || alt) {
        return ACT_NONE; /* Ctrl+Alt: the desktop's and the programs' own */
    }
    switch (key) {
    case K_F1: return shift ? ACT_NONE : ACT_HELP;
    case K_F1 + 1: return shift ? ACT_NONE : ACT_RENAME;
    case K_F1 + 2: return shift ? ACT_FIND_PREV : ACT_FIND_NEXT;
    case K_F1 + 4: return shift ? ACT_NONE : ACT_RELOAD;
    case K_F1 + 9: return shift ? ACT_CONTEXT_MENU : ACT_NONE; /* F10 alone is the main menu's, not an action */
    case '\t': return shift ? ACT_PREV_FIELD : ACT_NEXT_FIELD;
    case K_DEL: return shift ? ACT_DELETE_PERMANENT : ACT_DELETE;
    case K_INS: return shift ? ACT_PASTE : ACT_NONE;
    }
    return ACT_NONE;
}

int key_list_action(int key, int mods)
{
    int act = key_action(key, mods);
    if (act == ACT_NONE && key == 'q' && !(mods & (KEYS_CTRL | KEYS_ALT | KEYS_META | KEYS_ALTGR))) {
        act = ACT_CANCEL; /* in a list, q is Esc: never quit */
    }
    return act;
}
