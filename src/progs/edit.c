/*
 * edit [PATH...]: LOCachy's text editor, Kate's on the framebuffer. Its keys,
 * mouse, defaults and messages are those of Kate and KTextEditor (their
 * source: kate's apps/lib and addons/project, ktexteditor's src/view,
 * src/document, src/search, src/undo), for what is here: documents in tabs,
 * a vertical split, the Projects tree, tree-sitter highlighting, undo,
 * search and replace, go to line, line numbers, bracket matching, the
 * normal auto-indent, dynamic word wrap, the clipboard, Rime input and the
 * prompts for unsaved documents and files changed on disk. Every file is
 * read and written with the user's permissions (edit-file.c).
 *
 *   Ctrl+N new   Ctrl+O open   Ctrl+S save   Ctrl+Shift+S save as   Ctrl+L save all
 *   Ctrl+W close   Ctrl+Q quit   Alt+Left/Right, Ctrl+PgUp/PgDn: tabs   Alt+1..0: tab N
 *   Ctrl+Shift+L split   Ctrl+Shift+R close the split   F8, Shift+F8: the other split
 *   Ctrl+Alt+Shift+F: the sidebar (Projects)   Esc: close the bar, else deselect
 *   Ctrl+Z undo   Ctrl+Shift+Z redo   Ctrl+X/C/V (Shift+Del, Ctrl+Ins, Shift+Ins)
 *   Ctrl+Shift+Ins: paste the selection   Ctrl+A select all   Ctrl+Shift+A deselect
 *   Ctrl+F find   Ctrl+R replace   F3, Shift+F3: next, previous   Ctrl+H, Ctrl+Shift+H:
 *   find the selection   Ctrl+G go to line   Ctrl+D, Ctrl+Shift+D, Ctrl+/: comment,
 *   uncomment, toggle   Ctrl+I, Ctrl+Shift+I: indent   Ctrl+K delete line
 *   Ctrl+6, Ctrl+Shift+6: the matching bracket   Insert: overwrite
 *   F10 dynamic word wrap, F11 line numbers (Kate's handbook; its code has none)
 *   F6 the icon border (a click on it sets or clears the line's bookmark)
 *   Ctrl+Alt+O Quick Open   Ctrl+Tab, Ctrl+Shift+Tab: the last used documents
 *   Alt+Up, Alt+Down: the previous, next document of the Documents list
 *   Ctrl+B bookmark   Alt+PgUp, Alt+PgDown: the previous, next bookmark
 * The pointer: a click places the cursor, Shift extends, a double click a
 * word, a triple one a line, dragging selects, the middle button pastes the
 * selection, the wheel scrolls; tabs, the tree and the buttons click.
 *
 * Ctrl+Space (Kate: code completion) is the system's input method key, and
 * Ctrl+Alt+Esc process 1's: neither reaches edit.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wctype.h>

#include "base.h"
#include "clipboard.h"
#include "ime.h"
#include "files.h"
#include "edit-buffer.h"
#include "edit-doc.h"
#include "edit-file.h"
#include "edit-search.h"
#include "edit-syntax.h"
#include "edit-tree.h"
#include "edit-layout.h"
#include "font.h"
#include "edit-multicursor.h"

/* KTextEditor's defaults (src/utils/kateconfig.cpp). */
#define TAB_WIDTH     4     /* Tab Width */
#define INDENT_WIDTH  4     /* Indentation Width, with spaces (ReplaceTabsDyn) */
#define ALIGN_INDENT  80    /* Dynamic Word Wrap Align Indent, % of the width */
#define LINE_LIMIT    10000 /* Line Length Limit */
#define BRACKET_LINES 5000  /* how far a matching bracket is looked for */
#define WHEEL_LINES   3     /* QApplication::wheelScrollLines */
#define MAX_HIGHLIGHT 65536 /* Find All's and Replace All's highlights */
#define LINE_MODIFICATION 1 /* Line Modification: the markers in the border */
#define SCROLL_PREVIEW    1 /* Scroll Bar Preview: the text of the hovered place */
#define ICON_COLS     2     /* the icon border: the font's height wide (KateIconBorder::updateFont) */

/* Kate's Breeze Dark theme (KSyntaxHighlighting's breeze-dark.theme). */
#define C_BG        0x232629
#define C_TEXT      COLOUR_NORMAL
#define C_LINE      0x2A2E32 /* CurrentLine */
#define C_SELECT    0x2D5C76 /* TextSelection */
#define C_SEARCH    0x414857 /* SearchHighlight */
#define C_REPLACE   0x808021 /* ReplaceHighlight */
#define C_BRACKET   0x000000 /* BracketMatching */
#define C_BORDER    0x31363B /* IconBorder */
#define C_NUMBER    0x7A7C7D /* LineNumbers */
#define C_NUMBER_ON 0xA5A6A8 /* CurrentLineNumber */
#define C_BOOKMARK  0x0404BF /* MarkBookmark */
#define C_MODIFIED  0xC04900 /* ModifiedLines */
#define C_SAVED     0x1C8042 /* SavedLines */
#define C_SEPARATOR 0x3F4347
/* The window around it: Breeze Dark's. */
#define C_WINDOW    0x2A2E32
#define C_WTEXT     0xFCFCFC
#define C_DIM       0xA1A9B1
#define C_FOCUS     0x3DAEE9
#define C_INFO_BG   0x1D3E52
#define C_WARN_BG   0x4D3A1F
#define C_ERROR_BG  0x4D1F24
#define C_GOOD_BG   0x1F4D33
#define C_BAD_FIELD 0x5C2328
#define C_OK_FIELD  0x234D2E
#define C_FIELD     0x1B1E20

#define SIDE_DEFAULT 32
#define SIDE_W sidebar_width /* the tool view splitter's current width */
#define MAX_SPACES 4
#define MAX_ROWS   65536 /* visual rows of one line that are laid out */
#define MAX_NOTES  4

/* What clicks (ui_hit ids). */
#define HIT_TAB     100000 /* + space * 1000 + tab */
#define HIT_CLOSE   200000 /* + space * 1000 + tab: its close button */
#define HIT_TREE    300000 /* + row */
#define HIT_ACTION  400000 /* + space * 100 + note * 10 + action */
#define HIT_BAR     500000 /* + BAR_* item */
#define HIT_WELCOME 600000 /* + space * 10 + 0 new, 1 open */
#define HIT_STATUS  700000 /* + space * 10 + 0 cursor, 1 mode */
#define HIT_TABBAR  800000 /* + space: the empty part of a tab bar */
#define HIT_DIALOG  900000 /* + button */
#define HIT_MENU   1000000 /* + menu */
#define HIT_TREE_CLOSE 1000100
#define HIT_SIDE_TAB   1000200 /* + SIDE_*: the sidebar's tool view buttons */
#define HIT_DOCBAR     1000300 /* + button of the Documents toolbar */
#define HIT_SCROLL     1000400 /* + space: its scrollbar or mini-map */
#define HIT_SWITCHER   1001000 /* + row of the document switcher */
#define HIT_DOCROW     1100000 /* + row of the Documents list */
#define HIT_QUICKOPEN  1200000 /* + row of Quick Open's list */
/* The menus' Alt keys, as Kate's: &File, &Edit, Sele&ction, &Go, &View,
 * &LSP Client, &Tools, Sess&ions (edit-actions.inc). */
#define EDITOR_MNEMONICS "fecgvlti"
static void editor_menu(int menu);
static void draw_menubar(void);
static void language_changed(struct doc *d);
static void language_closed(struct doc *d);
static void language_saved(struct doc *d);
static int language_before_save(struct doc *d);
static int language_key(int key, int mods);
static void language_tick(void);
static int language_active(void);
static void language_done(void);
static void language_menu(void);
static void language_hover_at(struct doc *d, size_t at);
static int session_quit(void);
static void filesearch_menu(void);
/* edit-scrollbar.inc, edit-documents.inc, edit-quickopen.inc */
struct tab;
static void marks_moved(struct doc *d, size_t at, size_t removed, size_t inserted);
static void marks_free(struct doc *d);
static void lines_loaded(struct doc *d);
static void lines_saved(struct doc *d);
static void save_cleanups(struct doc *d);
static int bookmarked(struct doc *d, size_t line);
static void bookmark_set(struct doc *d, size_t line, int on);
static void bookmark_toggle(struct tab *t);
static void bookmark_step(int s, struct tab *t, int dir);
static void modified_line_step(int s, struct tab *t, int dir);
static uint32_t line_marker(struct doc *d, size_t line);
static int scroll_cols(int s, const struct tab *t);
static void draw_scrollbar(int s, struct tab *t);
static int scroll_pointer(int id, const struct pointer_event *e);
static int scroll_dragging(const struct pointer_event *e);
static void scroll_hover(int id, const struct pointer_event *e);
static void preview_draw(void);
static void preview_hide(void);
static int preview_timeout(void);
static void docs_draw(void);
static void docs_forget(struct doc *d);
static int docs_pointer(int id, const struct pointer_event *e);
static void docs_wheel(int steps);
static void docs_step(int dir);
static int switcher_key(int key, int mods);
static void switcher_draw(void);
static int switcher_pointer(int id, const struct pointer_event *e);
static void switcher_tick(void);
static int switcher_open(void);
static void quick_open(void);
static int auto_brackets(int s, struct tab *t, const char *text, size_t len, int *closing);
static void auto_bracket_close(struct tab *t, int closing);
static void auto_bracket_backspace(struct tab *t);
static void auto_bracket_check(void);

/* A KTextEditor::Message: posted to a document, shown in each of its views,
 * above the text until one of its actions (or Close) is chosen, or at the
 * bottom of the view until it hides itself. */
enum { NOTE_INFO, NOTE_WARNING, NOTE_ERROR, NOTE_POSITIVE };
enum { ACT_RELOAD = 1, ACT_AUTO_RELOAD, ACT_IGNORE, ACT_CLOSE_FILE, ACT_SAVE_AS, ACT_RAISE_LIMIT, ACT_CLOSE,
       ACT_TRY_AGAIN, ACT_REOPEN_ENCODING, ACT_READ_WRITE };
struct note {
    char text[768];
    int kind, above;
    double until;            /* when it hides itself; 0: when an action is chosen */
    int actions[4], nactions;
};

/* Line starts that move with the text as KTextEditor's marks move with their
 * lines (edit-scrollbar.inc), each with its flags; in order, one a line. */
struct anchors {
    size_t *at;
    unsigned char *flag;
    size_t n, size;
};

/* The program's own about a document. */
struct docui {
    struct anchors bookmarks; /* the marks of type 1 (markType01, Bookmark) */
    struct anchors lines;     /* the line modification system: LINE_MODIFIED, LINE_SAVED */
    struct anchors saved_lines; /* those flags at the last save or load, for undo back to it */
    int lines_restore;        /* undo or redo reached the saved state: saved_lines apply */
    size_t restore_done;      /* the undo position that reached it */
    int autobrace;            /* m_currentAutobraceRange is set: [ab_start, ab_end) */
    size_t ab_start, ab_end;
    int ab_char;              /* m_currentAutobraceClosingChar, or 0 */
    unsigned long version;    /* changed with each edit (the mini-map's cache) */
    long edited, raised;      /* when last edited, when last shown or created */
    char base[64];           /* KTextEditor's m_docName */
    int number;              /* m_docNameNumber */
    struct note notes[MAX_NOTES];
    int nnotes;
    int check;               /* inotify saw its name: look at the file */
    size_t limit;            /* the line length limit it was read with */
    size_t longest;
    size_t *hl;              /* Find All's or Replace All's ranges, pairs */
    size_t nhl;
    int hl_replace;
    int scope_valid;         /* power search's original selection, not its match */
    size_t scope_lo, scope_hi;
    int reopen_encoding;    /* 0: detect, else ENC_* + 1 */
    struct project *project;
};

/* A view of a document in a space (a KTextEditor::View in a view space). */
struct tab {
    struct doc *doc;
    size_t cursor, anchor;   /* anchor == cursor: no selection */
    size_t top;              /* the start of the first line shown, */
    int sub;                 /* and its first visual row shown */
    int left;                /* columns scrolled, without dynamic wrap */
    int goal;                /* Up's and Down's column (m_preservedX); -1: the cursor's */
    int numbers, wrap;       /* Show Line Numbers, Dynamic Word Wrap: on by default */
    int icons;               /* Show Icon Border: off by default */
    int scrollbar, minimap, marks; /* Show Scrollbars, Show Scrollbar Mini-Map, Show Scrollbar Marks */
    int font_px;             /* 0: system cell renderer, else independent content size */
    struct edit_multicursor *mc;
    struct edit_folding *folding;
    long used;               /* when last shown: the most recent comes next when one closes */
};

#include "edit-folding.inc"

/* A view space: its tabs and its share of the width. */
struct space {
    struct tab *tabs;
    int ntabs, size, cur;    /* cur -1: none (the welcome view) */
    int share;
};

static struct doc **docs;
static int ndocs;
static struct space spaces[MAX_SPACES];
static int nspaces = 1, active;
static struct edit_layout view_layout;
static struct edit_split *split_drag;
static struct edit_rect split_drag_area;
static int sidebar_width = SIDE_DEFAULT, sidebar_drag;
static struct content_font *editor_font;
static long use_clock;
static int quitting;

/* The sidebar: the Documents and Projects tool views, one shown at a time
 * (Kate's left sidebar), the one last shown first. At the first start (no
 * session to restore) Kate shows its default tool view, the Documents list
 * at the left of the editing area (the handbook's Quick Start); a session
 * keeps whether the sidebar is shown (Kate-MDI-ToolView-*-Visible). */
enum { SIDE_DOCUMENTS, SIDE_PROJECTS };
static int sidebar = 1, tree_focus, tree_sel, tree_top;
static int side_view = SIDE_DOCUMENTS;
static struct project *project;

/* What a new view shows at its sides (KTextEditor's defaults,
 * src/utils/kateconfig.cpp): Show Scrollbars AlwaysOn, Scroll Bar MiniMap
 * on, Scroll Bar Marks off, Icon Bar off. */
static int view_scrollbar_default = 1, view_minimap_default = 1, view_marks_default = 0, view_icons_default = 0;

/* The drag of the pointer's left button over the text. */
enum { DRAG_NONE, DRAG_CHARS, DRAG_WORDS, DRAG_LINES };
static struct {
    int mode, space;
    size_t start, end;       /* the word or line first selected */
    int y;                   /* the last pointer row, for scrolling past the edges */
} drag;
static int primary_pending;  /* a keyboard selection, for the primary selection when it ends */
static double primary_since;

/* A short notice of the input method's mode (as term's). */
static char toast[128];
static double toast_until;

static int ino = -1;         /* inotify */

static struct tab *tab_of(int s)
{
    struct space *sp = &spaces[s];
    return sp->cur >= 0 && sp->cur < sp->ntabs ? &sp->tabs[sp->cur] : NULL;
}

static struct tab *cur_tab(void)
{
    return tab_of(active);
}

static struct docui *ui_of(struct doc *d)
{
    return d->ui;
}

static double now(void)
{
    return uptime();
}

static size_t min_z(size_t a, size_t b)
{
    return a < b ? a : b;
}

static size_t max_z(size_t a, size_t b)
{
    return a > b ? a : b;
}

/* a with amount/255 of b over it (KColorUtils::mix, an alpha blend). */
static uint32_t colour_mix(uint32_t a, uint32_t b, int amount)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int x = (int)(a >> shift & 0xFF), y = (int)(b >> shift & 0xFF);
        out |= (uint32_t)((x * (255 - amount) + y * amount + 127) / 255) << shift;
    }
    return out;
}

static int has_sel(const struct tab *t)
{
    return t->anchor != t->cursor;
}

static void sel_range(const struct tab *t, size_t *a, size_t *b)
{
    *a = min_z(t->anchor, t->cursor);
    *b = max_z(t->anchor, t->cursor);
}

static size_t line_of(const struct doc *d, size_t at)
{
    return buffer_line_of(&d->buf, at);
}

static size_t line_start(const struct doc *d, size_t line)
{
    return buffer_line_start(&d->buf, line);
}

static size_t line_end(const struct doc *d, size_t line)
{
    return buffer_line_start(&d->buf, line) + buffer_line_length(&d->buf, line);
}

static size_t doc_len(const struct doc *d)
{
    return buffer_length(&d->buf);
}

static int byte_at(const struct doc *d, size_t at)
{
    return buffer_byte(&d->buf, at);
}

/* The key as typed: Ctrl with a letter as the letter, told apart from
 * Backspace, Tab and Enter (which give the same control characters). */
static int real_key(int key, int mods)
{
    if (!(mods & KEYS_CTRL)) {
        return key;
    }
    if (key == '\b' || key == '\t' || key == '\n') {
        char name[64];
        if (keys_last_name(name, sizeof name) == 0) {
            const char *k = strrchr(name, '+');
            k = k ? k + 1 : name;
            if (strlen(k) == 1 && isalpha((unsigned char)k[0])) {
                return tolower((unsigned char)k[0]);
            }
        }
        return key;
    }
    if (key >= 1 && key <= 26 && key != '\b' && key != '\t' && key != '\n') {
        return 'a' + key - 1;
    }
    return key;
}

/* A note for d's views. */
__attribute__((format(printf, 5, 6)))
static struct note *post(struct doc *d, int kind, int above, double seconds, const char *fmt, ...)
{
    struct docui *u = ui_of(d);
    if (u->nnotes == MAX_NOTES) {
        memmove(&u->notes[0], &u->notes[1], (MAX_NOTES - 1) * sizeof u->notes[0]);
        u->nnotes--;
    }
    struct note *n = &u->notes[u->nnotes++];
    memset(n, 0, sizeof *n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->text, sizeof n->text, fmt, ap);
    va_end(ap);
    n->kind = kind;
    n->above = above;
    n->until = seconds > 0 ? now() + seconds : 0;
    return n;
}

static void drop_note(struct doc *d, int i)
{
    struct docui *u = ui_of(d);
    memmove(&u->notes[i], &u->notes[i + 1], (size_t)(u->nnotes - i - 1) * sizeof u->notes[0]);
    u->nnotes--;
}

/* Notes with an action among them (the prompt for a file changed on disk). */
static void drop_notes_with(struct doc *d, int action)
{
    struct docui *u = ui_of(d);
    for (int i = u->nnotes - 1; i >= 0; i--) {
        for (int k = 0; k < u->notes[i].nactions; k++) {
            if (u->notes[i].actions[k] == action) {
                drop_note(d, i);
                break;
            }
        }
    }
}

/* An action of a note shown in the active view (its label's mnemonic). */
static const char *action_label(int a)
{
    switch (a) {
    case ACT_RELOAD: return _("&Reload");
    case ACT_AUTO_RELOAD: return _("Enable Auto Reload");
    case ACT_IGNORE: return _("&Ignore");
    case ACT_CLOSE_FILE: return _("&Close File");
    case ACT_SAVE_AS: return _("&Save As...");
    case ACT_RAISE_LIMIT: return _("Temporarily raise limit and reload file");
    case ACT_CLOSE: return _("&Close");
    case ACT_TRY_AGAIN: return _("Try Again");
    case ACT_REOPEN_ENCODING: return _("Reopen with &Encoding...");
    case ACT_READ_WRITE: return _("Enable Read-&Write Mode");
    }
    return "";
}

/* The text of a label without its &, and its mnemonic (0: none). */
static int label_text(const char *label, char *out, size_t size)
{
    int key = 0;
    size_t o = 0;
    for (const char *p = label; *p && o + 1 < size; p++) {
        if (*p == '&' && p[1] && p[1] != '&' && !key) {
            key = tolower((unsigned char)p[1]);
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return key;
}

/* KTextEditor's documentName: the file's name (with the folders that tell
 * it from another of the same name), or "Untitled", with the first line of
 * its text if it has one, and " (2)" and on for the same name again. */
static void doc_name(struct doc *d, char *out, size_t size)
{
    struct docui *u = ui_of(d);
    if (!d->path[0]) {
        if (u->base[0]) {
            snprintf(out, size, _("Untitled (%s)"), u->base);
        } else {
            snprintf(out, size, "%s", _("Untitled"));
        }
    } else {
        const char *base = strrchr(d->path, '/') + 1;
        /* The shortest end of the folder that the others of this name lack. */
        char prefix[PATH_MAX] = "";
        int same = 0;
        for (int i = 0; i < ndocs; i++) {
            if (docs[i] != d && docs[i]->path[0] && strcmp(strrchr(docs[i]->path, '/') + 1, base) == 0) {
                same = 1;
            }
        }
        if (same) {
            size_t dir_len = (size_t)(base - 1 - d->path);
            for (size_t k = dir_len; k > 0; k--) {
                if (k != dir_len && d->path[k - 1] != '/') {
                    continue;
                }
                const char *tail = d->path + (k == dir_len ? dir_len : k);
                int unique = 1;
                for (int i = 0; i < ndocs && unique; i++) {
                    const char *o = docs[i]->path;
                    if (docs[i] == d || !o[0] || strcmp(strrchr(o, '/') + 1, base) != 0) {
                        continue;
                    }
                    size_t ol = strlen(o), tl = strlen(tail);
                    unique = !(ol >= tl && strcmp(o + ol - tl, tail) == 0 &&
                               (ol == tl || o[ol - tl - 1] == '/'));
                }
                if (unique && k != dir_len) {
                    snprintf(prefix, sizeof prefix, "%.*s", (int)(dir_len - k), d->path + k);
                    break;
                }
            }
        }
        if (prefix[0]) {
            snprintf(out, size, "%s - %s", base, prefix);
        } else {
            snprintf(out, size, "%s", base);
        }
        return;
    }
    if (u->number > 0) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, " (%d)", u->number + 1);
    }
}

/* updateDocName for a document without a file: its first line, without
 * the characters Windows and XML do not allow in names, spaces made one,
 * 32 characters of it. */
static void update_name(struct doc *d)
{
    struct docui *u = ui_of(d);
    char base[64] = "";
    if (!d->path[0]) {
        size_t len = min_z(buffer_line_length(&d->buf, 0), 400), o = 0, chars = 0;
        char line[401];
        buffer_copy(&d->buf, 0, len, line);
        line[len] = '\0';
        int space = 1;
        for (size_t i = 0; i < len && chars < 32 && o + 4 < sizeof base;) {
            size_t k;
            text_decode(line + i, len - i, &k);
            char c = line[i];
            if (k == 1 && strchr("<>[]{}:\"/\\|?*#", c)) {
                i++;
                continue;
            }
            if (k == 1 && (c == ' ' || c == '\t')) {
                if (!space) {
                    base[o++] = ' ';
                    chars++;
                }
                space = 1;
                i++;
                continue;
            }
            memcpy(base + o, line + i, k);
            o += k;
            i += k;
            chars++;
            space = 0;
        }
        while (o > 0 && base[o - 1] == ' ') {
            o--;
        }
        base[o] = '\0';
    }
    if (strcmp(base, u->base) == 0 && (d->path[0] || u->number >= 0)) {
        return;
    }
    snprintf(u->base, sizeof u->base, "%s", base);
    int count = -1;
    for (int i = 0; i < ndocs; i++) {
        struct docui *o = ui_of(docs[i]);
        if (docs[i] != d && !docs[i]->path[0] == !d->path[0] && strcmp(o->base, u->base) == 0 && o->number > count) {
            count = o->number;
        }
    }
    u->number = d->path[0] ? 0 : count + 1;
}

/* A position after an edit of [at, at + removed) into inserted bytes. A
 * cursor moves on with text put where it is (Kate::TextCursor MoveOnInsert),
 * the ends of a selection and the top of a view stay. */
static size_t moved_pos(size_t p, size_t at, size_t removed, size_t inserted, int move_on_insert)
{
    if (p < at || (p == at && !(move_on_insert && removed == 0))) {
        return p;
    }
    if (p < at + removed) {
        return at;
    }
    return p - removed + inserted;
}

static void moved(struct doc *d, size_t at, size_t removed, size_t inserted)
{
    for (int s = 0; s < nspaces; s++) {
        for (int i = 0; i < spaces[s].ntabs; i++) {
            struct tab *t = &spaces[s].tabs[i];
            if (t->doc != d) {
                continue;
            }
            mc_moved(t, at, removed, inserted);
            fold_moved(t, at, removed, inserted);
            int sel = has_sel(t);
            t->cursor = moved_pos(t->cursor, at, removed, inserted, 1);
            t->anchor = sel ? moved_pos(t->anchor, at, removed, inserted, 0) : t->cursor;
            t->top = moved_pos(t->top, at, removed, inserted, 0);
            t->top = line_start(d, line_of(d, t->top));
        }
    }
    struct docui *u = ui_of(d);
    marks_moved(d, at, removed, inserted);
    if (u->scope_valid) {
        u->scope_lo = moved_pos(u->scope_lo, at, removed, inserted, 0);
        u->scope_hi = moved_pos(u->scope_hi, at, removed, inserted, 1);
    }
    if (u->nhl) {
        free(u->hl); /* the highlights of Find All are of the text before */
        u->hl = NULL;
        u->nhl = 0;
    }
    language_changed(d);
}

static struct doc *new_doc(void)
{
    struct doc *d = calloc(1, sizeof *d);
    struct docui *u = calloc(1, sizeof *u);
    struct doc **more = realloc(docs, (size_t)(ndocs + 1) * sizeof *more);
    if (!d || !u || !more || doc_init(d) < 0) {
        free(d);
        free(u);
        if (more) {
            docs = more;
        }
        return NULL;
    }
    docs = more;
    d->ui = u;
    d->moved = moved;
    u->number = -1;
    u->limit = LINE_LIMIT;
    u->raised = ++use_clock;
    docs[ndocs++] = d;
    update_name(d);
    return d;
}

static void unwatch(struct doc *d);

static void free_doc(struct doc *d)
{
    language_closed(d);
    for (int s = 0; s < nspaces; s++) {
        for (int i = 0; i < spaces[s].ntabs; i++) if (spaces[s].tabs[i].doc == d) {
            mc_free(&spaces[s].tabs[i]);
            fold_release(&spaces[s].tabs[i]);
        }
    }
    for (int i = 0; i < ndocs; i++) {
        if (docs[i] == d) {
            memmove(&docs[i], &docs[i + 1], (size_t)(ndocs - i - 1) * sizeof *docs);
            ndocs--;
            break;
        }
    }
    unwatch(d);
    struct docui *u = ui_of(d);
    free(u->hl);
    marks_free(d);
    docs_forget(d);
    free(u);
    doc_free(d);
    free(d);
}

/* The tab of d in space s, or -1. */
static int tab_index(int s, const struct doc *d)
{
    for (int i = 0; i < spaces[s].ntabs; i++) {
        if (spaces[s].tabs[i].doc == d) {
            return i;
        }
    }
    return -1;
}

static void show_tab(int s, int i)
{
    spaces[s].cur = i;
    if (i >= 0) {
        spaces[s].tabs[i].used = ++use_clock;
        ui_of(spaces[s].tabs[i].doc)->raised = use_clock; /* the switcher's last used */
    }
}

/* d shown in space s: its tab there, a new one at the end if it has none
 * (Kate's default: "Open new tabs to the right of the current tab" is off).
 * A new view of a document another view shows starts at its cursor. */
static struct tab *show_doc(int s, struct doc *d)
{
    struct space *sp = &spaces[s];
    int i = tab_index(s, d);
    if (i < 0) {
        if (sp->ntabs == sp->size) {
            int size = sp->size ? sp->size * 2 : 8;
            struct tab *tabs = realloc(sp->tabs, (size_t)size * sizeof *tabs);
            if (!tabs) {
                return tab_of(s);
            }
            sp->tabs = tabs;
            sp->size = size;
        }
        i = sp->ntabs++;
        struct tab *t = &sp->tabs[i];
        *t = (struct tab){ .doc = d, .goal = -1, .numbers = 1, .wrap = 1, .scrollbar = view_scrollbar_default,
                           .minimap = view_minimap_default, .marks = view_marks_default, .icons = view_icons_default };
        for (int o = 0; o < nspaces; o++) {
            int k = o == s ? -1 : tab_index(o, d);
            if (k >= 0) {
                t->cursor = t->anchor = spaces[o].tabs[k].cursor;
                break;
            }
        }
    }
    show_tab(s, i);
    return &sp->tabs[i];
}

/* The tab taken out of its space; the most recently used one shown. */
static void drop_tab(int s, int i)
{
    struct space *sp = &spaces[s];
    mc_free(&sp->tabs[i]);
    fold_release(&sp->tabs[i]);
    memmove(&sp->tabs[i], &sp->tabs[i + 1], (size_t)(sp->ntabs - i - 1) * sizeof *sp->tabs);
    sp->ntabs--;
    int best = -1;
    for (int k = 0; k < sp->ntabs; k++) {
        if (best < 0 || sp->tabs[k].used > sp->tabs[best].used) {
            best = k;
        }
    }
    show_tab(s, best);
}

static int doc_in_spaces(const struct doc *d)
{
    int n = 0;
    for (int s = 0; s < nspaces; s++) {
        n += tab_index(s, d) >= 0;
    }
    return n;
}

/* Space s taken away: its documents go to the active one (Kate's
 * removeViewSpace), its width to its neighbour. */
static void drop_space(int s)
{
    if (nspaces == 1) {
        return;
    }
    int to = s == active ? (s > 0 ? s - 1 : 1) : active;
    for (int i = 0; i < spaces[s].ntabs; i++) {
        if (tab_index(to, spaces[s].tabs[i].doc) < 0) {
            int keep = spaces[to].cur;
            show_doc(to, spaces[s].tabs[i].doc);
            spaces[to].cur = keep;
        }
    }
    if (view_layout.root) edit_layout_remove(&view_layout, s);
    split_drag = NULL;
    spaces[to].share += spaces[s].share;
    for (int i = 0; i < spaces[s].ntabs; i++) {
        mc_free(&spaces[s].tabs[i]);
        fold_release(&spaces[s].tabs[i]);
    }
    free(spaces[s].tabs);
    memmove(&spaces[s], &spaces[s + 1], (size_t)(nspaces - s - 1) * sizeof spaces[0]);
    nspaces--;
    memset(&spaces[nspaces], 0, sizeof spaces[0]);
    if (to > s) {
        to--;
    }
    active = to;
    if (spaces[active].cur < 0 && spaces[active].ntabs > 0) {
        show_tab(active, 0);
    }
}

/*
 * Where things are: row 0 is the top bar, row 1 the tab bars, the last row
 * the status bars; between them each space has its notes, its text, and
 * (the active one) the search or go-to bar.
 */
enum { BAR_NONE, BAR_FIND, BAR_POWER, BAR_GOTO };
static int bar_kind;

static int text_x0(void)
{
    return sidebar ? SIDE_W + 1 : 0;
}

static struct edit_rect editor_area(void)
{
    int x = text_x0();
    return (struct edit_rect){ x, 2, screen_cols() - x, screen_rows() - 2 };
}

static struct edit_rect space_rect(int s)
{
    struct edit_rect r = editor_area();
    if (view_layout.root && edit_layout_rect(&view_layout, s, r, &r)) return r;
    int total = r.w - (nspaces - 1), shares = 0, before = 0;
    for (int i = 0; i < nspaces; i++) {
        int share = spaces[i].share > 0 ? spaces[i].share : 1;
        shares += share;
        if (i < s) before += share;
    }
    int a = (int)((long)total * before / shares);
    int b = (int)((long)total * (before + (spaces[s].share > 0 ? spaces[s].share : 1)) / shares);
    r.x += a + s;
    r.w = b - a;
    return r;
}

/* Space s's columns. */
static void space_cols(int s, int *x, int *w)
{
    struct edit_rect r = space_rect(s);
    *x = r.x;
    *w = r.w;
}

static int bar_rows(int s)
{
    return s != active ? 0 : bar_kind == BAR_POWER ? 2 : bar_kind ? 1 : 0;
}

static int note_rows_of(const struct note *n, int w);

/* The rows of the notes above space s's text. */
static int notes_above(int s)
{
    struct tab *t = tab_of(s);
    int x, w, rows = 0;
    if (!t) {
        return 0;
    }
    space_cols(s, &x, &w);
    struct docui *u = ui_of(t->doc);
    for (int i = 0; i < u->nnotes; i++) {
        if (u->notes[i].above) {
            rows += note_rows_of(&u->notes[i], w);
        }
    }
    int max = (space_rect(s).h - 3) / 2;
    if (max < 0) max = 0;
    return rows < max ? rows : max;
}

static int content_metrics(int s, int *cw, int *ch, int *top)
{
    screen_cells(cw, ch, top);
    struct tab *t = tab_of(s);
    if (!t || !t->font_px) return 0;
    if (!editor_font) editor_font = content_font_new(NULL, t->font_px);
    if (!editor_font || content_font_set_size_px(editor_font, t->font_px) < 0) return 0;
    content_font_metrics(editor_font, cw, ch, NULL);
    return *cw > 0 && *ch > 0;
}

/* UI rows stay in system cells; the document gets its own font metrics. */
static int text_first_row(int s)
{
    return space_rect(s).y + 1 + notes_above(s);
}

/* Space s's text rows: from *y, *h visual document lines. */
static void text_rows(int s, int *y, int *h)
{
    struct edit_rect r = space_rect(s);
    *y = text_first_row(s);
    *h = r.y + r.h - 1 - bar_rows(s) - *y;
    int base_w, base_h, top, cw, ch;
    screen_cells(&base_w, &base_h, &top);
    if (content_metrics(s, &cw, &ch, &top)) *h = *h * base_h / ch;
    if (*h < 1) {
        *h = 1;
    }
}

static int digits(size_t n)
{
    int k = 1;
    while (n >= 10) {
        n /= 10;
        k++;
    }
    return k;
}

/* The border at the left of the text (KateIconBorder::paintBorder): the
 * icon border when shown (ICON_COLS), a column for the folding markers (in
 * Kate they follow the modification markers; here they keep the column they
 * had before it), the line numbers, then the line modification markers (Line
 * Modification, for a document with a file) right after the numbers, in the
 * column that is the space before the text. */
static int gutter(const struct tab *t)
{
    int icons = t->icons ? ICON_COLS : 0;
    if (t->numbers) return icons + (digits(t->doc->buf.lines) > 2 ? digits(t->doc->buf.lines) : 2) + 2;
    return icons + 1 + (LINE_MODIFICATION && t->doc->path[0]);
}

static int text_width(int s, const struct tab *t)
{
    int x, w;
    space_cols(s, &x, &w);
    w -= scroll_cols(s, t); /* the scrollbar's or the mini-map's columns at the right */
    int base_w, base_h, top, cw, ch;
    screen_cells(&base_w, &base_h, &top);
    if (content_metrics(s, &cw, &ch, &top)) w = w * base_w / cw;
    int tw = w - gutter(t);
    return tw > 2 ? tw : 2;
}

/* The cells of character cp at column x of a row. */
static int cells_at(uint32_t cp, int x)
{
    if (cp == '\t') {
        return TAB_WIDTH - x % TAB_WIDTH;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return 1;
    }
    return screen_char_cells(cp);
}

/* The character cluster at at (a character and its combining marks, not
 * past end), its cells at column x, and where the next one starts. */
static size_t cluster(const struct doc *d, size_t at, size_t end, int x, int *cells, uint32_t *first)
{
    size_t next = buffer_next_cluster(&d->buf, at), n;
    next = next > end ? end : next;
    *cells = 0;
    *first = buffer_char(&d->buf, at, &n);
    for (size_t p = at; p < next;) {
        uint32_t cp = buffer_char(&d->buf, p, &n);
        *cells += cells_at(cp, x + *cells);
        p += n ? n : 1;
    }
    return next;
}

static int is_wide(uint32_t cp)
{
    return screen_char_cells(cp) == 2;
}

/* Line's visual rows in width cells: where each starts, how many there are
 * (at most max), and *shift, the cells continuation rows are indented by.
 * Without dynamic word wrap a line is one row. With it, as Kate's renderer
 * lays it out (QTextLayout, WrapAtWordBoundaryOrAnywhere): after spaces,
 * which stay at the end of their row, or around a wide (CJK) character, else
 * anywhere; continuation rows indented as the line is when that is at most
 * ALIGN_INDENT% of the width. */
static int layout(const struct tab *t, int width, size_t line, size_t *starts, int max, int *shift)
{
    const struct doc *d = t->doc;
    size_t ls = line_start(d, line), le = line_end(d, line);
    starts[0] = ls;
    *shift = 0;
    if (!t->wrap) {
        return 1;
    }
    int lead = 0;
    for (size_t p = ls; p < le; p++) {
        int c = byte_at(d, p);
        if (c != ' ' && c != '\t') {
            lead = p > ls;
            break;
        }
    }
    if (lead) {
        int x = 0;
        for (size_t p = ls; p < le && (byte_at(d, p) == ' ' || byte_at(d, p) == '\t'); p++) {
            x += cells_at((uint32_t)byte_at(d, p), x);
        }
        *shift = x * 100 <= width * ALIGN_INDENT ? x : 0;
    }
    int n = 1, x = 0, row_w = width;
    size_t brk = 0;
    for (size_t at = ls; at < le;) {
        int w;
        uint32_t cp;
        size_t next = cluster(d, at, le, x, &w, &cp);
        int space = cp == ' ' || cp == '\t', wide = is_wide(cp);
        if (wide && at > starts[n - 1]) {
            brk = at;
        }
        if (!space && x + w > row_w && x > 0) {
            if (n == max) {
                break;
            }
            size_t cut = brk > starts[n - 1] ? brk : at;
            starts[n++] = cut;
            row_w = width - *shift > 1 ? width - *shift : width;
            x = 0;
            for (size_t p = cut; p < at;) {
                int pw;
                uint32_t pc;
                p = cluster(d, p, le, x, &pw, &pc);
                x += pw;
            }
            brk = 0;
            continue;
        }
        x += w;
        if (space || wide) {
            brk = next;
        }
        at = next;
    }
    return n;
}

static size_t row_buf[MAX_ROWS];

static int row_of(const size_t *starts, int n, size_t at)
{
    int r = n - 1;
    while (r > 0 && starts[r] > at) {
        r--;
    }
    return r;
}

/* The cells from from to to on one row (x from 0). */
static int cells_between(const struct doc *d, size_t from, size_t to)
{
    int x = 0;
    for (size_t p = from; p < to;) {
        int w;
        uint32_t cp;
        p = cluster(d, p, to, x, &w, &cp);
        x += w;
    }
    return x;
}

/* Where at is shown: the visual row of its line, and its column. */
static void place_of(const struct tab *t, int width, size_t at, int *row, int *x)
{
    size_t line = line_of(t->doc, at);
    int shift, n = layout(t, width, line, row_buf, MAX_ROWS, &shift);
    *row = row_of(row_buf, n, at);
    *x = (*row > 0 ? shift : 0) + cells_between(t->doc, row_buf[*row], at);
}

/* The position on row r of line at column x: the character there (Up and
 * Down), or with boundary the place between characters nearest to x (a
 * click, x in cells counted to the nearest edge). A wrapped row ends before
 * its last character, which would be shown on the next row. */
static size_t at_column(const struct tab *t, int width, size_t line, int r, int x, int boundary)
{
    const struct doc *d = t->doc;
    int shift, n = layout(t, width, line, row_buf, MAX_ROWS, &shift);
    r = r < 0 ? 0 : r >= n ? n - 1 : r;
    size_t from = row_buf[r], end = r + 1 < n ? row_buf[r + 1] : line_end(d, line);
    int base = r > 0 ? shift : 0, cx = base;
    size_t last = from;
    for (size_t p = from; p < end;) {
        int w;
        uint32_t cp;
        size_t next = cluster(d, p, end, cx - base, &w, &cp);
        if (!boundary && x < cx + w) {
            return p;
        }
        if (boundary && x < cx + w) {
            return x - cx <= cx + w - x ? p : (next < end || r + 1 >= n ? next : p);
        }
        cx += w;
        last = p;
        p = next;
    }
    if (r + 1 < n) {
        return last > from ? last : from;
    }
    return end;
}

static int line_rows(const struct tab *t, int width, size_t line)
{
    int shift;
    return layout(t, width, line, row_buf, MAX_ROWS, &shift);
}

/* (line, row) moved by delta visual rows, within the text. */
static void move_rows(const struct tab *t, int width, size_t *line, int *row, long delta)
{
    size_t lines = t->doc->buf.lines;
    while (delta > 0) {
        int n = line_rows(t, width, *line);
        if (*row + delta < n) {
            *row += (int)delta;
            return;
        }
        size_t next = fold_next(t, *line);
        if (next >= lines) {
            *row = n - 1;
            return;
        }
        delta -= n - *row;
        *line = next;
        *row = 0;
    }
    while (delta < 0) {
        if (*row + delta >= 0) {
            *row += (int)delta;
            return;
        }
        if (*line == 0) {
            *row = 0;
            return;
        }
        delta += *row + 1;
        *line = fold_prev(t, *line);
        *row = line_rows(t, width, *line) - 1;
    }
}

/* Kate does not scroll past the end: the last row at the bottom at most. */
static void clamp_top(int s, struct tab *t)
{
    int y, h, width = text_width(s, t);
    text_rows(s, &y, &h);
    size_t line = fold_line(t, t->doc->buf.lines - 1), top = fold_line(t, line_of(t->doc, t->top));
    int row = line_rows(t, width, line) - 1;
    move_rows(t, width, &line, &row, -(h - 1));
    if (top > line || (top == line && t->sub > row)) {
        t->top = line_start(t->doc, line);
        t->sub = row;
    }
    int n = line_rows(t, width, line_of(t->doc, t->top));
    if (t->sub >= n) {
        t->sub = n - 1;
    }
    if (t->sub < 0) {
        t->sub = 0;
    }
}

/* The cursor in view: scrolled as little as needed, or with its line in
 * the middle (a jump: go to line, a search). */
static void show_cursor(int s, struct tab *t, int center)
{
    fold_reveal(t, line_of(t->doc, t->cursor));
    int y, h, width = text_width(s, t), row, x;
    text_rows(s, &y, &h);
    place_of(t, width, t->cursor, &row, &x);
    size_t cl = line_of(t->doc, t->cursor), tl = line_of(t->doc, t->top);
    int above = cl < tl || (cl == tl && row < t->sub);
    int below = 0;
    if (!above) {
        /* Rows from the top to the cursor, as far as the view goes. */
        size_t line = tl;
        int r = t->sub;
        long k = 0;
        while (k < h && (line < cl || (line == cl && r < row))) {
            size_t pl = line;
            int pr = r;
            move_rows(t, width, &line, &r, 1);
            if (line == pl && r == pr) {
                break;
            }
            k++;
        }
        below = k >= h;
    }
    if (above || below) {
        size_t line = cl;
        int r = row;
        move_rows(t, width, &line, &r, center ? -(h / 2) : above ? 0 : -(h - 1));
        t->top = line_start(t->doc, line);
        t->sub = r;
    }
    if (!t->wrap) {
        int tw = width;
        if (x < t->left) {
            t->left = center ? (x > tw / 2 ? x - tw / 2 : 0) : x;
        } else if (x >= t->left + tw) {
            t->left = center ? x - tw / 2 : x - tw + 1;
        }
    } else {
        t->left = 0;
    }
    clamp_top(s, t);
}

/*
 * The search bars and the go-to bar (KateSearchBar, KateGotoBar), at the
 * bottom of the active view. Their text fields are edited as line edits.
 */
struct field {
    char s[1024];
    int len, cur; /* bytes */
    int all;      /* all of it selected: what is typed replaces it */
};

enum { FOCUS_VIEW, FOCUS_BAR };
/* What can have the focus in a bar, in its Tab order. */
enum { ITEM_FIND, ITEM_REPLACE, ITEM_MODE, ITEM_CASE, ITEM_SELECTION, ITEM_LINE,
       ITEM_NEXT, ITEM_PREV, ITEM_REPLACE_ONE, ITEM_REPLACE_ALL, ITEM_FIND_ALL, ITEM_GO, ITEM_POWER, ITEM_CLOSE };
enum { MATCH_NOTHING, MATCH_FOUND, MATCH_WRAPPED_TOP, MATCH_WRAPPED_BOTTOM, MATCH_MISMATCH, MATCH_INVALID };

static struct {
    int focus;               /* FOCUS_VIEW or FOCUS_BAR (on item) */
    int item;
    struct field find, replace, line;
    int inc_case;            /* the incremental bar's Match case: off by default */
    int mode, power_case, selection_only; /* the power bar's: plain text, case-sensitive */
    size_t inc_start;        /* where the incremental search started (m_incInitCursor) */
    int match;               /* MATCH_*: the status label and the field's colour */
    char error[160];         /* an invalid regular expression's reason */
    struct search s;
    int counter;
    int last;                /* the kind of the bar last shown: F3 searches as it did */
} bar = { .power_case = 1, .last = BAR_FIND };

static const char *const mode_names[] = { N_("Plain text"), N_("Whole words"), N_("Escape sequences"), N_("Regular expression") };

/* Drawing. */
static void put_text(int col, int row, int max_col, const char *s, uint32_t fg, uint32_t bg)
{
    char buf[1024];
    size_t o = 0;
    int x = col;
    for (const char *p = s; *p && x < max_col && o + 5 < sizeof buf;) {
        size_t k;
        uint32_t cp = text_decode(p, strlen(p), &k);
        int w = screen_char_cells(cp);
        if (x + w > max_col) {
            break;
        }
        memcpy(buf + o, p, k);
        o += k;
        p += k;
        x += w;
    }
    buf[o] = '\0';
    screen_text(col, row, buf, fg, bg);
}

static int text_cells(const char *s)
{
    int n = 0;
    for (const char *p = s; *p;) {
        size_t k;
        uint32_t cp = text_decode(p, strlen(p), &k);
        n += screen_char_cells(cp);
        p += k;
    }
    return n;
}

/* The bytes of s that fit in max, backed up so that no character is split:
 * the precision of a %.*s for text that may be UTF-8 (translated or a name). */
static int utf8_fit(const char *s, int max)
{
    int n = (int)strnlen(s, (size_t)max + 1);
    if (n <= max) {
        return n;
    }
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

static int tab_label(const struct tab *t, char *out, size_t size)
{
    char name[256];
    doc_name(t->doc, name, sizeof name);
    snprintf(out, size, " %s%s ", doc_modified(t->doc) ? "*" : "", name);
    return text_cells(out);
}

static void draw_tabbar(int s)
{
    int x0, w;
    space_cols(s, &x0, &w);
    struct space *sp = &spaces[s];
    int row = space_rect(s).y;
    screen_fill(x0, row, w, 1, C_WINDOW);
    ui_hit(x0, row, w, 1, HIT_TABBAR + s);
    /* From the first tab that leaves the current one in view. */
    int first = 0;
    if (sp->cur >= 0) {
        int used = 0;
        for (int i = sp->cur; i >= 0; i--) {
            char label[300];
            used += tab_label(&sp->tabs[i], label, sizeof label) + 2;
            if (used > w) {
                break;
            }
            first = i;
        }
    }
    int x = x0;
    for (int i = first; i < sp->ntabs && x < x0 + w; i++) {
        char label[300];
        int n = tab_label(&sp->tabs[i], label, sizeof label);
        int on = i == sp->cur;
        uint32_t bg = on ? (s == active ? C_BG : C_BORDER) : C_WINDOW, fg = on ? C_WTEXT : C_DIM;
        int end = x + n + 2 < x0 + w ? x + n + 2 : x0 + w;
        screen_fill(x, row, end - x, 1, bg);
        put_text(x, row, end, label, fg, bg);
        ui_hit(x, row, end - x, 1, HIT_TAB + s * 1000 + i);
        if (x + n + 1 < x0 + w) {
            screen_text(x + n, row, "x", on ? C_WTEXT : C_DIM, bg); /* its close button */
            ui_hit(x + n, row, 1, 1, HIT_CLOSE + s * 1000 + i);
        }
        x = end;
    }
}

/* A note's rows at width w: its words, then its buttons. With draw, it is
 * drawn from (x, y), its buttons clickable as id + k. */
static int note_layout(const struct note *n, int x, int y, int w, int draw, int id)
{
    static const uint32_t bgs[] = { C_INFO_BG, C_WARN_BG, C_ERROR_BG, C_GOOD_BG };
    uint32_t bg = bgs[n->kind];
    int col = 1, row = 0;
    if (draw) {
        screen_fill(x, y, w, 1, bg);
    }
    /* The text, word by word ("<br />" as a line break, as Kate's rich text). */
    char text[800];
    size_t o = 0;
    for (const char *p = n->text; *p && o + 2 < sizeof text; p++) {
        if (strncmp(p, "<br />", 6) == 0 || strncmp(p, "<br/>", 5) == 0) {
            text[o++] = '\n';
            p += p[3] == ' ' ? 5 : 4;
        } else {
            text[o++] = *p;
        }
    }
    text[o] = '\0';
    for (char *p = text; *p;) {
        if (*p == '\n') {
            row++;
            col = 1;
            p++;
            if (draw) {
                screen_fill(x, y + row, w, 1, bg);
            }
            continue;
        }
        size_t len = strcspn(p, " \n");
        char word[800];
        snprintf(word, sizeof word, "%.*s", (int)len, p);
        int cells = text_cells(word);
        if (col > 1 && col + cells > w - 1) {
            row++;
            col = 1;
            if (draw) {
                screen_fill(x, y + row, w, 1, bg);
            }
        }
        if (draw) {
            put_text(x + col, y + row, x + w, word, C_WTEXT, bg);
        }
        col += cells + 1;
        p += len;
        if (*p == ' ') {
            p++;
        }
    }
    for (int k = 0; k < n->nactions; k++) {
        char label[64];
        label_text(action_label(n->actions[k]), label, sizeof label);
        int cells = text_cells(label) + 2;
        if (col > 1 && col + cells > w - 1) {
            row++;
            col = 1;
            if (draw) {
                screen_fill(x, y + row, w, 1, bg);
            }
        }
        if (draw) {
            ui_button(x + col, y + row, label, id + k);
        }
        col += cells + 1;
    }
    return row + 1;
}

static int note_rows_of(const struct note *n, int w)
{
    return note_layout(n, 0, 0, w, 0, 0);
}

/* The matching bracket's position for the cursor (KTextEditor's
 * findMatchingBracket): the bracket after the cursor, else the one before
 * (in overwrite mode only the one after), the match of the same colour,
 * within BRACKET_LINES lines. 1 with both, else 0. */
static int bracket_pair(const struct tab *t, size_t *a, size_t *b);

/* The colour of the text from from to to (syntax highlighting). */
static uint32_t *colours_of(struct doc *d, size_t from, size_t to)
{
    uint32_t *c = malloc((to - from + 1) * sizeof *c);
    if (!c) {
        return NULL;
    }
    if (d->syntax && to > from) {
        syntax_colours(d->syntax, &d->buf, from, to, c);
    } else {
        for (size_t i = 0; i <= to - from; i++) {
            c[i] = C_TEXT;
        }
    }
    return c;
}

/* Inside a Find All or Replace All range (u->hl pairs, in order). */
static int in_highlight(const struct docui *u, size_t at)
{
    size_t lo = 0, hi = u->nhl;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (u->hl[2 * mid + 1] <= at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < u->nhl && u->hl[2 * lo] <= at && at < u->hl[2 * lo + 1];
}

static int cell_px(int *w, int *h, int *top)
{
    screen_cells(w, h, top);
    return *w > 0;
}

/* The cursor: a line at the left of its cell, or the cell in overwrite
 * mode (Kate's caret styles). */
static void draw_caret(int col, int row, int overwrite, const char *glyph, uint32_t bg)
{
    int cw, ch, top;
    if (overwrite) {
        screen_fill(col, row, 1, 1, C_TEXT);
        screen_text(col, row, glyph, bg, C_TEXT);
    } else if (cell_px(&cw, &ch, &top)) {
        int pw = cw / 6 > 2 ? cw / 6 : 2;
        screen_fill_px(col * cw, top + row * ch, pw, ch, C_TEXT);
    }
}

/* Translate only document painting; menus, tabs and the shared bar retain
 * their established size and proportions. */
static int painting_space = -1;
static unsigned painting_style;
static void content_fill(int x, int y, int w, int h, uint32_t bg)
{
    int cw, ch, top;
    if (painting_space < 0 || !content_metrics(painting_space, &cw, &ch, &top)) {
        screen_fill(x, y, w, h, bg);
        return;
    }
    int ux, uw, bw, bh, base_top;
    space_cols(painting_space, &ux, &uw);
    screen_cells(&bw, &bh, &base_top);
    int first = text_first_row(painting_space);
    screen_fill_px(ux * bw + (x - ux) * cw, base_top + first * bh + (y - first) * ch, w * cw, h * ch, bg);
}

static void content_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    int cw, ch, top;
    if (painting_space < 0 || !content_metrics(painting_space, &cw, &ch, &top)) {
        screen_text(x, y, text, fg, bg);
        return;
    }
    int ux, uw, bw, bh, base_top, first = text_first_row(painting_space), rows, ignored;
    space_cols(painting_space, &ux, &uw);
    screen_cells(&bw, &bh, &base_top);
    text_rows(painting_space, &ignored, &rows);
    int py = base_top + first * bh;
    content_font_draw(editor_font, ux * bw + (x - ux) * cw, py + (y - first) * ch, text,
                      ux * bw, py, uw * bw, rows * ch, fg, bg, painting_style);
}

static void content_put_text(int x, int y, int max_x, const char *text, uint32_t fg, uint32_t bg)
{
    char clipped[2048];
    size_t used = 0;
    int col = x;
    for (const char *p = text; *p && used + 5 < sizeof clipped;) {
        size_t k;
        uint32_t cp = text_decode(p, strlen(p), &k);
        int width = screen_char_cells(cp);
        if (col + width > max_x) break;
        memcpy(clipped + used, p, k);
        used += k;
        p += k;
        col += width;
    }
    clipped[used] = '\0';
    content_text(x, y, clipped, fg, bg);
}

static void content_caret(int x, int y, int overwrite, const char *glyph, uint32_t bg)
{
    int cw, ch, top;
    if (painting_space < 0 || !content_metrics(painting_space, &cw, &ch, &top)) {
        draw_caret(x, y, overwrite, glyph, bg);
        return;
    }
    int ux, uw, bw, bh, base_top, first = text_first_row(painting_space);
    space_cols(painting_space, &ux, &uw);
    screen_cells(&bw, &bh, &base_top);
    if (overwrite) {
        content_fill(x, y, 1, 1, C_TEXT);
        content_text(x, y, glyph, bg, C_TEXT);
    } else {
        screen_fill_px(ux * bw + (x - ux) * cw, base_top + first * bh + (y - first) * ch,
                       cw / 6 > 2 ? cw / 6 : 2, ch, C_TEXT);
    }
}

#define screen_fill content_fill
#define screen_text content_text
#define put_text content_put_text
#define draw_caret content_caret
static void draw_text(int s)
{
    struct tab *t = tab_of(s);
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    int x0, w, y0, h;
    space_cols(s, &x0, &w);
    text_rows(s, &y0, &h);
    w -= scroll_cols(s, t);
    painting_space = s;
    int cw, ch, pixel_top, bw, bh;
    screen_cells(&bw, &bh, &pixel_top);
    if (content_metrics(s, &cw, &ch, &pixel_top)) w = w * bw / cw;
    int g = gutter(t), tw = w - g, focus = s == active && bar.focus == FOCUS_VIEW && !tree_focus;
    size_t cl = line_of(d, t->cursor), sa, sb, ba = (size_t)-1, bb = (size_t)-1;
    sel_range(t, &sa, &sb);
    bracket_pair(t, &ba, &bb);
    /* The rows shown first, for the colours of all of them at once. */
    struct shown {
        size_t line, from, to;
        int row, last, shift;
    } *rows = calloc((size_t)h, sizeof *rows);
    if (!rows) {
        painting_space = -1;
        return;
    }
    size_t line = fold_line(t, line_of(d, t->top));
    int r = t->sub, n = 0, shift = 0;
    while (n < h && line < d->buf.lines) {
        int count = layout(t, tw, line, row_buf, MAX_ROWS, &shift);
        for (; r < count && n < h; r++, n++) {
            rows[n] = (struct shown){ line, row_buf[r], r + 1 < count ? row_buf[r + 1] : line_end(d, line), r,
                                      r + 1 == count, shift };
        }
        line = fold_next(t, line);
        r = 0;
    }
    uint32_t *colour = n ? colours_of(d, rows[0].from, rows[n - 1].to) : NULL;
    size_t base = n ? rows[0].from : 0;
    size_t style_length = n ? rows[n - 1].to - base : 0;
    uint8_t *styles = style_length ? calloc(style_length, 1) : NULL;
    if (styles && d->syntax) syntax_styles(d->syntax, &d->buf, base, base + style_length, styles);
    painting_style = 0;
    screen_fill(x0, y0, w, h, C_BG);
    screen_fill(x0, y0, g, h, C_BORDER);
    for (int i = 0; i < n; i++) {
        struct shown *v = &rows[i];
        int y = y0 + i;
        uint32_t row_bg = v->line == cl ? C_LINE : C_BG;
        if (bookmarked(d, v->line)) {
            row_bg = colour_mix(row_bg, C_BOOKMARK, 26); /* KateRenderer: a tenth of the mark's colour */
        }
        screen_fill(x0 + g, y, tw, 1, row_bg);
        int ic = t->icons ? ICON_COLS : 0;
        if (t->numbers && v->row == 0) {
            char num[32];
            snprintf(num, sizeof num, "%*zu", g - ic - 2, v->line + 1);
            screen_text(x0 + ic + 1, y, num, v->line == cl ? C_NUMBER_ON : C_NUMBER, C_BORDER);
        }
        if (v->row == 0) {
            int marker = fold_marker(t, v->line);
            if (marker) {
                char sign[2] = { (char)marker, 0 };
                screen_text(x0 + ic, y, sign, C_FOCUS, C_BORDER);
            }
            /* The icon border: the bookmark's mark on its line's first row. */
            if (ic && bookmarked(d, v->line)) screen_fill(x0, y, ic, 1, C_BOOKMARK);
        }
        /* The line modification markers, on each row of the line: a bar in
         * ModifiedLines' or SavedLines' colour. */
        uint32_t modified = line_marker(d, v->line);
        if (modified) screen_text(x0 + g - 1, y, "▌", modified, C_BORDER);
        int x = (v->row > 0 ? v->shift : 0) - t->left, rx = 0;
        char run[2048];
        size_t ro = 0;
        int run_x = x;
        uint32_t run_fg = 0, run_bg = 0;
        unsigned run_style = 0;
        for (size_t p = v->from; p <= v->to; ) {
            int cw = 0;
            uint32_t cp = 0;
            size_t next = p < v->to ? cluster(d, p, v->to, rx, &cw, &cp) : p;
            uint32_t fg = colour && p < v->to ? colour[p - base] : C_TEXT;
            uint32_t bg = row_bg;
            if (mc_selected(t, p)) {
                bg = C_SELECT;
            } else if (u->nhl && in_highlight(u, p)) {
                bg = u->hl_replace ? C_REPLACE : C_SEARCH;
            } else if (p == ba || p == bb) {
                bg = C_BRACKET;
            }
            int end = p >= v->to;
            unsigned style = styles && p < v->to ? styles[p - base] : 0;
            if (end || fg != run_fg || bg != run_bg || style != run_style || ro + 64 > sizeof run) {
                if (ro) {
                    run[ro] = '\0';
                    if (run_x >= 0) {
                        painting_style = run_style;
                        put_text(x0 + g + run_x, y, x0 + w, run, run_fg, run_bg);
                        painting_style = 0;
                    }
                }
                ro = 0;
                run_x = x;
                run_fg = fg;
                run_bg = bg;
                run_style = style;
            }
            if (end) {
                break;
            }
            /* What screen_text is given: tabs as spaces, a control
             * character or a byte that is no character as ?. */
            if (x >= 0 && x + cw <= tw) {
                if (!ro) {
                    run_x = x; /* skipped off-screen characters start no run */
                }
                if (cp == '\t') {
                    for (int k = 0; k < cw; k++) {
                        run[ro++] = ' ';
                    }
                } else {
                    for (size_t q = p; q < next;) {
                        size_t k;
                        uint32_t c = buffer_char(&d->buf, q, &k);
                        if (c < 0x20 || c == 0x7F || (k == 1 && byte_at(d, q) >= 0x80)) {
                            run[ro++] = '?';
                        } else {
                            ro += (size_t)text_put(run + ro, c);
                        }
                        q += k ? k : 1;
                    }
                }
            } else if (x < 0 && x + cw > 0) {
                run_x = x + cw; /* a character cut by the left edge is not drawn */
            }
            x += cw;
            rx += cw;
            p = next;
        }
        /* A selected line break: the selection goes on to the right edge. */
        if (v->last && mc_selected(t, v->to) && v->to < doc_len(d)) {
            int from = x < 0 ? 0 : x;
            if (from < tw) {
                screen_fill(x0 + g + from, y, tw - from, 1, C_SELECT);
            }
        }
    }
    /* The cursor, in the view that has the focus. */
    if (focus) {
        int crow, cx;
        place_of(t, tw, t->cursor, &crow, &cx);
        for (int i = 0; i < n; i++) {
            if (rows[i].line == cl && rows[i].row == crow) {
                int col = cx - t->left;
                if (col >= 0 && col < tw) {
                    char glyph[8] = " ";
                    size_t k;
                    uint32_t c = buffer_char(&d->buf, t->cursor, &k);
                    if (k && c >= 0x20 && c != 0x7F && t->cursor < line_end(d, cl)) {
                        glyph[text_put(glyph, c)] = '\0';
                    }
                            draw_caret(x0 + g + col, y0 + i, d->overwrite, glyph,
                               t->cursor >= sa && t->cursor < sb ? C_SELECT : rows[i].line == cl ? C_LINE : C_BG);
                }
                break;
            }
        }
    }
    mc_draw_carets(s, t, x0 + g, y0, tw, h, focus);
    free(styles);
    free(colour);
    free(rows);
    /* Notes at the bottom of the view (Search wrapped, N replacements made). */
    int k = 0;
    for (int i = u->nnotes - 1; i >= 0 && k < h; i--) {
        const struct note *nt = &u->notes[i];
        if (!nt->above) {
            int len = text_cells(nt->text) + 2, col = x0 + (w > len ? (w - len) / 2 : 0);
            static const uint32_t bgs[] = { C_INFO_BG, C_WARN_BG, C_ERROR_BG, C_GOOD_BG };
            screen_fill(col, y0 + h - 1 - k, len < w ? len : w, 1, bgs[nt->kind]);
            put_text(col + 1, y0 + h - 1 - k, x0 + w, nt->text, C_WTEXT, bgs[nt->kind]);
            k++;
        }
    }
    painting_space = -1;
    draw_scrollbar(s, t);
}
#undef screen_fill
#undef screen_text
#undef put_text
#undef draw_caret

/* Kate's virtual column: tabs to the next stop, a character one (and one
 * past the BMP two, as UTF-16 counts them). */
static size_t virtual_column(const struct doc *d, size_t at)
{
    size_t col = 0;
    for (size_t p = line_start(d, line_of(d, at)); p < at;) {
        size_t k;
        uint32_t c = buffer_char(&d->buf, p, &k);
        col = c == '\t' ? (col / TAB_WIDTH + 1) * TAB_WIDTH : col + (c >= 0x10000 ? 2 : 1);
        p += k ? k : 1;
    }
    return col;
}

/* KTextEditor's status bar (katestatusbar.cpp): right-aligned, the cursor
 * (its compact form, line:column), INSERT or OVERWRITE, the tabs, the
 * encoding, the line ends (Kate hides them by default; LOCachy's plan
 * shows them) and the mode, with the modified mark (Kate's window title's
 * [*]) before them. */
static void draw_status(int s)
{
    struct edit_rect r = space_rect(s);
    int x0, w, row = r.y + r.h - 1;
    space_cols(s, &x0, &w);
    screen_fill(x0, row, w, 1, C_WINDOW);
    struct tab *t = tab_of(s);
    if (!t) {
        return;
    }
    struct doc *d = t->doc;
    char cursor[48], tabs[64];
    snprintf(cursor, sizeof cursor, "%zu:%zu", line_of(d, t->cursor) + 1, virtual_column(d, t->cursor) + 1);
    if (mc_count(t) > 1 || mc_block(t)) {
        size_t n = mc_count(t);
        snprintf(tabs, sizeof tabs, mc_block(t) ? P_("%zu cursor [BLOCK]", "%zu cursors [BLOCK]", n) :
                                                  P_("%zu cursor", "%zu cursors", n), n);
    } else {
        snprintf(tabs, sizeof tabs, _("Soft Tabs: %d"), INDENT_WIDTH);
    }
    const char *items[] = { doc_modified(d) ? "*" : "", cursor, d->overwrite ? _("OVERWRITE") : _("INSERT"), tabs,
                            file_encoding_name(&d->format), file_eol_name(d->format.eol),
                            syntax_name(d->lang) };
    int total = 0;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        total += items[i][0] ? text_cells(items[i]) + 3 : 0;
    }
    int x = x0 + w - total;
    x = x < x0 ? x0 : x;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (!items[i][0]) {
            continue;
        }
        int n = text_cells(items[i]);
        put_text(x + 1, row, x0 + w, items[i], i == 0 ? C_FOCUS : C_WTEXT, C_WINDOW);
        if (i == 1 || i == 2) {
            ui_hit(x, row, n + 2, 1, HIT_STATUS + s * 10 + (i == 1 ? 0 : 1));
        }
        x += n + 3;
    }
}

static void draw_field(int col, int row, int width, const struct field *f, int focused, uint32_t bg)
{
    screen_fill(col, row, width, 1, bg);
    /* Scrolled so that its cursor shows. */
    int before = 0;
    for (int i = 0; i < f->cur;) {
        size_t k;
        uint32_t cp = text_decode(f->s + i, (size_t)(f->len - i), &k);
        before += screen_char_cells(cp);
        i += (int)k;
    }
    int skip = before >= width ? before - width + 1 : 0, x = 0;
    const char *p = f->s;
    while (*p && x < skip) {
        size_t k;
        uint32_t cp = text_decode(p, strlen(p), &k);
        x += screen_char_cells(cp);
        p += k;
    }
    put_text(col, row, col + width, p, C_WTEXT, bg);
    if (focused) {
        draw_caret(col + before - skip, row, 0, " ", bg);
    }
}

static int bar_button(int col, int row, int max, const char *label, int item, int enabled)
{
    char text[64];
    label_text(label, text, sizeof text);
    int n = text_cells(text) + 2;
    if (col + n > max) {
        return col;
    }
    uint32_t bg = bar.focus == FOCUS_BAR && bar.item == item ? C_FOCUS : 0x304860;
    screen_fill(col, row, n, 1, bg);
    screen_text(col, row, "[", C_WTEXT, bg);
    put_text(col + 1, row, col + n - 1, text, enabled ? C_WTEXT : C_DIM, bg);
    screen_text(col + n - 1, row, "]", C_WTEXT, bg);
    if (enabled) {
        ui_hit(col, row, n, 1, HIT_BAR + item);
    }
    return col + n + 1;
}

/* A check box: [x] label. */
static int bar_toggle(int col, int row, int max, const char *label, int on, int item)
{
    char text[64];
    snprintf(text, sizeof text, "[%c] %s", on ? 'x' : ' ', label);
    int n = text_cells(text);
    if (col + n > max) {
        return col;
    }
    uint32_t bg = bar.focus == FOCUS_BAR && bar.item == item ? C_FOCUS : C_WINDOW;
    screen_fill(col, row, n, 1, bg);
    put_text(col, row, col + n, text, C_WTEXT, bg);
    ui_hit(col, row, n, 1, HIT_BAR + item);
    return col + n + 1;
}

static const char *match_text(void)
{
    switch (bar.match) {
    case MATCH_WRAPPED_TOP: return _("Reached top, continued from bottom");
    case MATCH_WRAPPED_BOTTOM: return _("Reached bottom, continued from top");
    case MATCH_MISMATCH: return _("Not found");
    case MATCH_INVALID: return bar.error;
    }
    return "";
}

static int pattern_ok(void);

static void draw_bar(void)
{
    if (!bar_kind) {
        return;
    }
    struct edit_rect r = space_rect(active);
    int x0, w, row = r.y + r.h - 1 - bar_rows(active), end;
    space_cols(active, &x0, &w);
    end = x0 + w;
    screen_fill(x0, row, w, bar_rows(active), C_WINDOW);
    uint32_t field_bg = bar.match == MATCH_MISMATCH || bar.match == MATCH_INVALID ? C_BAD_FIELD :
                        bar.match == MATCH_FOUND || bar.match == MATCH_WRAPPED_TOP ||
                        bar.match == MATCH_WRAPPED_BOTTOM ? C_OK_FIELD : C_FIELD;
    int fw = w / 4 > 12 ? w / 4 : 12, ok = pattern_ok(), c;
    if (bar_kind == BAR_GOTO) {
        char of[48];
        const char *line_lbl = _("Line:");
        screen_text(x0 + 1, row, line_lbl, C_WTEXT, C_WINDOW);
        int lx = x0 + 2 + ui_cells(line_lbl);
        draw_field(lx, row, 12, &bar.line, bar.focus == FOCUS_BAR && bar.item == ITEM_LINE, C_FIELD);
        ui_hit(lx, row, 12, 1, HIT_BAR + ITEM_LINE);
        /* TRANSLATORS: Total line count in Go to Line bar, e.g. "Line: [12] of 100". */
        snprintf(of, sizeof of, _("of %zu"), tab_of(active) ? tab_of(active)->doc->buf.lines : 0);
        screen_text(lx + 13, row, of, C_DIM, C_WINDOW);
        bar_button(lx + 14 + text_cells(of), row, end, _("Go to"), ITEM_GO, 1);
        return;
    }
    const char *find_lbl = _("Find:"), *rep_lbl = _("Replace:");
    screen_text(x0 + 1, row, find_lbl, C_WTEXT, C_WINDOW);
    int lw = ui_cells(find_lbl);
    if (bar_kind == BAR_POWER && ui_cells(rep_lbl) > lw) lw = ui_cells(rep_lbl);
    int fx = x0 + 2 + lw;
    draw_field(fx, row, fw, &bar.find, bar.focus == FOCUS_BAR && bar.item == ITEM_FIND,
               bar_kind == BAR_FIND || bar.match ? field_bg : C_FIELD);
    ui_hit(fx, row, fw, 1, HIT_BAR + ITEM_FIND);
    c = fx + fw + 1;
    c = bar_button(c, row, end, _("Next"), ITEM_NEXT, ok);
    c = bar_button(c, row, end, _("Previous"), ITEM_PREV, ok);
    if (bar_kind == BAR_POWER) {
        char mode[48];
        snprintf(mode, sizeof mode, _("&Mode: %s"), _(mode_names[bar.mode]));
        c = bar_button(c, row, end, mode, ITEM_MODE, 1);
    }
    int match_case = bar_kind == BAR_FIND ? bar.inc_case : bar.power_case;
    c = bar_toggle(c, row, end, _("Match case"), match_case, ITEM_CASE);
    if (bar_kind == BAR_POWER) {
        c = bar_toggle(c, row, end, _("Selection only"), bar.selection_only, ITEM_SELECTION);
    } else {
        c = bar_button(c, row, end, _("Replace..."), ITEM_POWER, 1);
        put_text(c, row, end, match_text(), bar.match == MATCH_MISMATCH || bar.match == MATCH_INVALID ? 0xFF9090 : C_DIM,
                 C_WINDOW);
        return;
    }
    row++;
    screen_text(x0 + 1, row, rep_lbl, C_WTEXT, C_WINDOW);
    draw_field(fx, row, fw, &bar.replace, bar.focus == FOCUS_BAR && bar.item == ITEM_REPLACE, C_FIELD);
    ui_hit(fx, row, fw, 1, HIT_BAR + ITEM_REPLACE);
    c = fx + fw + 1;
    c = bar_button(c, row, end, _("&Replace"), ITEM_REPLACE_ONE, ok);
    c = bar_button(c, row, end, _("Replace &All"), ITEM_REPLACE_ALL, ok);
    c = bar_button(c, row, end, _("&Find All"), ITEM_FIND_ALL, ok);
    put_text(c, row, end, match_text(), bar.match == MATCH_MISMATCH || bar.match == MATCH_INVALID ? 0xFF9090 : C_DIM,
             C_WINDOW);
}

static struct node *tree_nodes[8192];
static int tree_depth[8192];

static void tree_clamp(int reveal)
{
    int n = tree_rows(project, tree_nodes, tree_depth, 8192);
    int h = screen_rows() - 3;
    if (h < 1) h = 1;
    tree_sel = !n ? -1 : tree_sel < 0 ? 0 : tree_sel >= n ? n - 1 : tree_sel;
    if (reveal && tree_sel >= 0) {
        if (tree_sel < tree_top) tree_top = tree_sel;
        if (tree_sel >= tree_top + h) tree_top = tree_sel - h + 1;
    }
    int max = n > h ? n - h : 0;
    tree_top = tree_top < 0 ? 0 : tree_top > max ? max : tree_top;
}

/* Keep the selected path and first visible path through insertions/deletions;
 * drawing alone must never pull a wheel-scrolled view back to selection. */
static void refresh_project(void)
{
    if (!project) return;
    char selected[PATH_MAX] = "", first[PATH_MAX] = "";
    int n = tree_rows(project, tree_nodes, tree_depth, 8192);
    if (tree_sel >= 0 && tree_sel < n) node_path(project, tree_nodes[tree_sel], selected, sizeof selected);
    if (tree_top >= 0 && tree_top < n) node_path(project, tree_nodes[tree_top], first, sizeof first);
    project_refresh(project);
    n = tree_rows(project, tree_nodes, tree_depth, 8192);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        node_path(project, tree_nodes[i], path, sizeof path);
        if (strcmp(path, selected) == 0) tree_sel = i;
        if (strcmp(path, first) == 0) tree_top = i;
    }
    tree_clamp(0);
}

/* The sidebar's row of tool view buttons (Kate's sidebar tabs): Documents,
 * then Projects with its project's name; the one shown is pressed. */
static void draw_side_tabs(void)
{
    char projects[PATH_MAX + 16];
    snprintf(projects, sizeof projects, _("Projects: %s"), project && project->top ? project->top->name : "");
    const char *labels[] = { _("Documents"), projects };
    int x = 1;
    for (int i = 0; i < 2 && x < SIDE_W - 2; i++) {
        int n = text_cells(labels[i]), on = side_view == i;
        uint32_t bg = on ? C_BORDER : C_WINDOW;
        int end = x + n < SIDE_W - 2 ? x + n : SIDE_W - 2;
        screen_fill(x, 2, end - x, 1, bg);
        put_text(x, 2, end, labels[i], on ? C_WTEXT : C_DIM, bg);
        ui_hit(x, 2, end - x, 1, HIT_SIDE_TAB + i);
        x = end + 2;
    }
}

static void draw_sidebar(void)
{
    int rows = screen_rows();
    screen_fill(0, 2, SIDE_W, rows - 2, C_WINDOW);
    screen_fill(SIDE_W, 2, 1, rows - 2, C_SEPARATOR);
    draw_side_tabs();
    if (side_view == SIDE_DOCUMENTS) {
        docs_draw();
        return;
    }
    int n = tree_rows(project, tree_nodes, tree_depth, 8192), h = rows - 3;
    tree_clamp(0);
    for (int i = tree_top; i < n && i < tree_top + h; i++) {
        struct node *nd = tree_nodes[i];
        int on = i == tree_sel;
        uint32_t bg = on ? (tree_focus ? C_FOCUS : C_BORDER) : C_WINDOW;
        char line[400];
        snprintf(line, sizeof line, "%*s%s%s", 1 + 2 * tree_depth[i], "", nd->dir ? (nd->expanded ? "- " : "+ ") : "  ",
                 nd->name);
        screen_fill(0, 3 + i - tree_top, SIDE_W, 1, bg);
        put_text(0, 3 + i - tree_top, SIDE_W, line, on && tree_focus ? C_WINDOW : nd->dir ? C_FOCUS : C_WTEXT, bg);
        ui_hit(0, 3 + i - tree_top, SIDE_W, 1, HIT_TREE + i);
    }
}

/* Kate's welcome view, for a space without documents. */
static void draw_welcome(int s)
{
    int x0, w, y0, h;
    space_cols(s, &x0, &w);
    text_rows(s, &y0, &h);
    screen_fill(x0, y0 - 1, w, h + 1, C_BG);
    int y = y0 + h / 3;
    put_text(x0 + 2, y, x0 + w, _("No document is open."), C_WTEXT, C_BG);
    int c = ui_button(x0 + 2, y + 2, _("New File"), HIT_WELCOME + s * 10);
    ui_button(c, y + 2, _("Open File..."), HIT_WELCOME + s * 10 + 1);
}

static void draw(void)
{
    int cols = screen_cols(), rows = screen_rows();
    ui_hits_clear();
    screen_fill(0, 0, cols, rows, C_WINDOW);
    if (sidebar) {
        draw_sidebar();
    }
    for (int s = 0; s < nspaces; s++) {
        int x0, w;
        space_cols(s, &x0, &w);
        struct edit_rect r = space_rect(s);
        if (r.x > text_x0()) screen_fill(r.x - 1, r.y, 1, r.h, C_SEPARATOR);
        if (r.y > 2) screen_fill(r.x, r.y - 1, r.w, 1, C_SEPARATOR);
        draw_tabbar(s);
        struct tab *t = tab_of(s);
        if (!t) {
            draw_welcome(s);
            draw_status(s);
            continue;
        }
        int y = r.y + 1;
        struct docui *u = ui_of(t->doc);
        for (int i = 0; i < u->nnotes; i++) {
            if (u->notes[i].above) {
                y += note_layout(&u->notes[i], x0, y, w, 1, HIT_ACTION + s * 100 + i * 10);
            }
        }
        draw_text(s);
        draw_status(s);
    }
    draw_bar();
    preview_draw();
    char row1[512], row2[512];
    int composing = ime_get_overlay(row1, row2, sizeof row1);
    if (composing) {
        if (row2[0] && rows >= 3) {
            screen_fill(0, rows - 2, cols, 1, 0x182848);
            screen_text(1, rows - 2, row1, 0xFFFFFF, 0x182848);
            screen_fill(0, rows - 1, cols, 1, 0x182848);
            screen_text(1, rows - 1, row2, 0xB8D8FF, 0x182848);
        } else {
            screen_fill(0, rows - 1, cols, 1, 0x182848);
            screen_text(1, rows - 1, row1, 0xFFFFFF, 0x182848);
        }
    }
    if (toast[0] && now() < toast_until) {
        int n = text_cells(toast), col = cols > n ? (cols - n) / 2 : 0;
        screen_fill(col, rows - 4, n, 1, 0x183860);
        screen_text(col, rows - 4, toast, 0xFFFFFF, 0x183860);
    }
    draw_menubar();
    switcher_draw();
    screen_bar("edit", ime_mode_label());
    screen_flush();
}

/*
 * Editing, as KTextEditor's view and document do it.
 */

/* Characters of a word for double clicks and word moves (KateHighlighting's
 * isInWord with KSyntaxHighlighting's default delimiters), and those of
 * WordCursor (letters, digits, _). */
static int in_word(uint32_t c)
{
    return !(c < 0x80 && strchr(" \t.():!+,-<=>%&*/;?[]^{|}~\\\"'`", (int)c)) && c != '\n' && !iswspace((wint_t)c);
}

static int word_char(uint32_t c)
{
    return c == '_' || iswalnum((wint_t)c);
}

static int is_space(uint32_t c)
{
    return c == ' ' || c == '\t' || (c >= 0x80 && iswspace((wint_t)c));
}

/* Line's characters (code points) and where each starts. */
struct chars {
    uint32_t *cp;
    size_t *at;
    size_t n;
};

static void line_chars(const struct doc *d, size_t line, struct chars *c)
{
    size_t ls = line_start(d, line), le = line_end(d, line);
    c->cp = malloc((le - ls + 1) * sizeof *c->cp);
    c->at = malloc((le - ls + 1) * sizeof *c->at);
    c->n = 0;
    for (size_t p = ls; c->cp && c->at && p < le;) {
        size_t k;
        c->at[c->n] = p;
        c->cp[c->n++] = buffer_char(&d->buf, p, &k);
        p += k ? k : 1;
    }
    if (c->at) {
        c->at[c->n] = le;
    }
}

static void chars_free(struct chars *c)
{
    free(c->cp);
    free(c->at);
}

static size_t char_index(const struct chars *c, size_t at)
{
    size_t i = 0;
    while (i < c->n && c->at[i] < at) {
        i++;
    }
    return i;
}

/* KateViewInternal::wordNext. */
static size_t word_next(const struct doc *d, size_t at)
{
    size_t line = line_of(d, at);
    if (at >= line_end(d, line)) {
        return line + 1 < d->buf.lines ? line_start(d, line + 1) : at;
    }
    struct chars c;
    line_chars(d, line, &c);
    if (!c.cp || !c.at) {
        chars_free(&c);
        return at;
    }
    size_t i = char_index(&c, at), j = i;
    if (in_word(c.cp[i])) {
        /* WordCursor: the word, then spaces; at least one character. */
        while (j < c.n && word_char(c.cp[j])) {
            j++;
        }
        if (j < c.n && is_space(c.cp[j])) {
            while (j < c.n && is_space(c.cp[j])) {
                j++;
            }
        }
        j = j == i ? i + 1 : j;
    } else {
        while (j < c.n && !in_word(c.cp[j]) && !is_space(c.cp[j])) {
            j++;
        }
        while (j < c.n && is_space(c.cp[j])) {
            j++;
        }
    }
    size_t r = c.at[j];
    chars_free(&c);
    return r;
}

/* KateViewInternal::wordPrev. */
static size_t word_prev(const struct doc *d, size_t at)
{
    size_t line = line_of(d, at);
    struct chars c;
    line_chars(d, line, &c);
    if (!c.cp || !c.at) {
        chars_free(&c);
        return at;
    }
    size_t col = char_index(&c, at), i = col;
    while (i > 0 && is_space(c.cp[i - 1])) {
        i--;
    }
    size_t r;
    if (i == 0) {
        r = line > 0 && col == 0 ? line_end(d, line - 1) : c.at[0];
        if (line > 0 && col == 0) {
            chars_free(&c);
            return r;
        }
    }
    if (i > 0 && in_word(c.cp[i - 1])) {
        /* WordCursor backwards, from the cursor. */
        long k = (long)(col < c.n ? col : c.n - 1) - 1;
        if (col == c.n && k + 1 >= 0 && k + 1 < (long)c.n && iswalpha((wint_t)c.cp[k + 1]) && k >= 0 &&
            is_space(c.cp[k])) {
            k++;
        }
        if (k > 0 && is_space(c.cp[k])) {
            while (k > 0 && is_space(c.cp[k])) {
                k--;
            }
        }
        for (long m = k; m > 0; m--) {
            if (!word_char(c.cp[m])) {
                break;
            }
            k--;
        }
        if (k >= 0 && !word_char(c.cp[k])) {
            k++;
        }
        size_t jump = k < 0 ? 0 : (size_t)k == col && col > 0 ? col - 1 : (size_t)k;
        r = c.at[jump];
    } else if (i > 0) {
        while (i > 0 && !in_word(c.cp[i - 1]) && !is_space(c.cp[i - 1])) {
            i--;
        }
        r = c.at[i];
    } else {
        r = c.at[0];
    }
    chars_free(&c);
    return r;
}

/* KTextEditor's wordRangeAt: the run of word characters around at. */
static void word_at(const struct doc *d, size_t at, size_t *a, size_t *b)
{
    size_t line = line_of(d, at);
    struct chars c;
    line_chars(d, line, &c);
    *a = *b = at;
    if (c.cp && c.at) {
        size_t i = char_index(&c, at), s = i, e = i;
        while (s > 0 && in_word(c.cp[s - 1])) {
            s--;
        }
        while (e < c.n && in_word(c.cp[e])) {
            e++;
        }
        *a = c.at[s];
        *b = c.at[e];
    }
    chars_free(&c);
}

/* The first character that is not a space, or -1 (Kate's firstChar). */
static long first_char(const struct doc *d, size_t line)
{
    size_t ls = line_start(d, line), le = line_end(d, line);
    for (size_t p = ls; p < le; p++) {
        int c = byte_at(d, p);
        if (c != ' ' && c != '\t') {
            return (long)(p - ls);
        }
    }
    return -1;
}

/* A move of the cursor by the user: the selection follows it or ends, and
 * the edit before it can no more grow (undoCancel's safe point). */
static void move_to(int s, struct tab *t, size_t at, int sel, int keep_goal)
{
    t->cursor = at;
    if (!sel) {
        t->anchor = at;
    } else {
        primary_pending = 1;
        primary_since = now();
    }
    if (!keep_goal) {
        t->goal = -1;
    }
    doc_safe_point(t->doc);
    show_cursor(s, t, 0);
}

static void set_primary(struct tab *t)
{
    if (mc_copy(t, 0, 1)) return;
    if (!t || !has_sel(t)) {
        return;
    }
    size_t a, b;
    sel_range(t, &a, &b);
    char *text = buffer_dup(&t->doc->buf, a, b - a);
    if (text) {
        clipboard_set(1, text, b - a);
        free(text);
    }
}

static int writable(struct doc *d)
{
    return !d->readonly;
}

static void remove_selection(struct tab *t)
{
    size_t a, b;
    sel_range(t, &a, &b);
    doc_remove(t->doc, a, b - a);
    t->cursor = t->anchor = a;
}

/* After an edit of the first line, a document without a file is named again. */
static void edited(struct tab *t)
{
    if (!t->doc->path[0]) {
        update_name(t->doc);
    }
}

/* KTextEditor's typeChars: over the selection, or in overwrite mode over as
 * many characters as are typed (not past the line's end); a tab as spaces
 * to the next indentation stop (ReplaceTabsDyn). */
static void type_text(int s, struct tab *t, const char *text, size_t len)
{
    if (mc_type_text(s, t, text, len)) return;
    struct doc *d = t->doc;
    if (!writable(d) || len == 0) {
        return;
    }
    int closing = 0; /* Auto Brackets: the closing one to add after the text */
    if (auto_brackets(s, t, text, len, &closing)) {
        return;
    }
    char *spaces = NULL;
    if (len == 1 && text[0] == '\t') {
        size_t col = virtual_column(d, has_sel(t) ? min_z(t->anchor, t->cursor) : t->cursor);
        size_t n = INDENT_WIDTH - col % INDENT_WIDTH;
        spaces = malloc(n);
        if (!spaces) {
            return;
        }
        memset(spaces, ' ', n);
        text = spaces;
        len = n;
    }
    doc_begin(d, t->cursor, t->anchor);
    if (has_sel(t)) {
        remove_selection(t);
    } else if (d->overwrite) {
        size_t le = line_end(d, line_of(d, t->cursor)), end = t->cursor;
        for (size_t i = 0; i < len && end < le;) {
            size_t k;
            text_decode(text + i, len - i, &k);
            end = min_z(buffer_next_cluster(&d->buf, end), le);
            i += k;
        }
        doc_remove(d, t->cursor, end - t->cursor);
    }
    doc_insert(d, t->cursor, text, len);
    if (closing) {
        auto_bracket_close(t, closing);
    }
    t->anchor = t->cursor;
    doc_end(d, t->cursor, t->anchor);
    free(spaces);
    edited(t);
    show_cursor(s, t, 0);
}

/* KateAutoIndent::keepIndent: line's leading space as the nearest line
 * above it that is not empty has it. */
static void keep_indent(struct doc *d, size_t line)
{
    long prev = (long)line - 1;
    while (prev >= 0 && buffer_line_length(&d->buf, (size_t)prev) == 0) {
        prev--;
    }
    if (prev < 0) {
        return;
    }
    size_t ps = line_start(d, (size_t)prev), pe = ps;
    while (pe < line_end(d, (size_t)prev) && (byte_at(d, pe) == ' ' || byte_at(d, pe) == '\t')) {
        pe++;
    }
    char *ws = buffer_dup(&d->buf, ps, pe - ps);
    size_t ls = line_start(d, line), le = ls;
    while (le < line_end(d, line) && (byte_at(d, le) == ' ' || byte_at(d, le) == '\t')) {
        le++;
    }
    doc_remove(d, ls, le - ls);
    if (ws) {
        doc_insert(d, ls, ws, pe - ps);
    }
    free(ws);
}

enum { NEWLINE_INDENT, NEWLINE_PLAIN, NEWLINE_SMART, NEWLINE_ABOVE, NEWLINE_BELOW };

/* Return (the normal indenter), Ctrl+Return (no indent), Shift+Return (the
 * line's leading non-word characters again), Ctrl+Alt+Return and
 * Ctrl+Shift+Return (a line above or below). */
static void newline(int s, struct tab *t, int how)
{
    if (mc_newline(s, t, how)) return;
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    doc_begin(d, t->cursor, t->anchor);
    if (how == NEWLINE_SMART) {
        size_t line = line_of(d, t->cursor), ls = line_start(d, line);
        long first = first_char(d, line), col = (long)(t->cursor - ls);
        size_t le = line_end(d, line), keep;
        if (first >= 0) {
            size_t p = ls + (size_t)(first < col ? first : col);
            while (p < le && p < t->cursor) {
                size_t k;
                uint32_t c = buffer_char(&d->buf, p, &k);
                if (iswalnum((wint_t)c) || c == '_') {
                    break;
                }
                p += k ? k : 1;
            }
            keep = p - ls;
        } else {
            keep = le - ls;
        }
        char *lead = buffer_dup(&d->buf, ls, keep);
        doc_insert(d, t->cursor, "\n", 1);
        if (lead) {
            doc_insert(d, line_start(d, line + 1), lead, keep);
        }
        free(lead);
    } else {
        if (has_sel(t)) {
            remove_selection(t);
        }
        size_t line = line_of(d, t->cursor);
        if (how == NEWLINE_BELOW) {
            t->cursor = line_end(d, line);
        } else if (how == NEWLINE_ABOVE) {
            t->cursor = line > 0 ? line_end(d, line - 1) : 0;
        }
        size_t at = t->cursor;
        doc_insert(d, at, "\n", 1);
        if (how == NEWLINE_ABOVE && line == 0) {
            t->cursor = 0;
        }
        if (how != NEWLINE_PLAIN) {
            keep_indent(d, line_of(d, t->cursor));
        }
    }
    t->anchor = t->cursor;
    doc_end(d, t->cursor, t->anchor);
    edited(t);
    show_cursor(s, t, 0);
}

/* The indentation of a line in cells, and its leading space's bytes. */
static int indent_depth(const struct doc *d, size_t line, size_t *bytes)
{
    size_t ls = line_start(d, line), p = ls;
    int x = 0;
    while (p < line_end(d, line) && (byte_at(d, p) == ' ' || byte_at(d, p) == '\t')) {
        x = byte_at(d, p) == '\t' ? (x / TAB_WIDTH + 1) * TAB_WIDTH : x + 1;
        p++;
    }
    *bytes = p - ls;
    return x;
}

/* KateAutoIndent::doIndentRelative: by change cells, to a multiple of the
 * indentation width, in spaces. */
static void indent_relative(struct doc *d, size_t line, int change)
{
    size_t old;
    int depth = indent_depth(d, line, &old), extra = depth % INDENT_WIDTH;
    depth += change;
    if (extra > 0) {
        depth += change < 0 ? INDENT_WIDTH - extra : -extra;
    }
    depth = depth < 0 ? 0 : depth;
    char *ws = malloc((size_t)depth + 1);
    if (!ws) {
        return;
    }
    memset(ws, ' ', (size_t)depth);
    size_t ls = line_start(d, line);
    char *now_ws = buffer_dup(&d->buf, ls, old);
    if (now_ws && (old != (size_t)depth || memcmp(now_ws, ws, old) != 0)) {
        /* The new one put in before the old one is taken out, as Kate does. */
        doc_insert(d, ls, ws, (size_t)depth);
        doc_remove(d, ls + (size_t)depth, old);
    }
    free(now_ws);
    free(ws);
}

/* KateAutoIndent::changeIndent over [a, b]: empty lines left as they are
 * (unless all are), nor the last line when b is at its start. */
static void change_indent(struct tab *t, size_t a, size_t b, int change)
{
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    size_t first = line_of(d, a), last = line_of(d, b), skipped = 0;
    doc_begin(d, t->cursor, t->anchor);
    for (size_t l = first; l <= last; l++) {
        if (buffer_line_length(&d->buf, l) == 0 || (l == last && b == line_start(d, l))) {
            skipped++;
            continue;
        }
        indent_relative(d, l, change * INDENT_WIDTH);
    }
    if (skipped > last - first) {
        for (size_t l = first; l <= last; l++) {
            indent_relative(d, l, change * INDENT_WIDTH);
        }
    }
    doc_end(d, t->cursor, t->anchor);
}

static void indent_selection(struct tab *t, int change)
{
    if (mc_indent(t, change)) return;
    size_t a, b;
    if (has_sel(t)) {
        sel_range(t, &a, &b);
    } else {
        a = b = line_start(t->doc, line_of(t->doc, t->cursor));
    }
    change_indent(t, a, b, change);
}

/* Tab (tabSmart): a selection over lines, or the cursor in a line's
 * leading space, indents; otherwise a tab is typed. */
static void key_tab(int s, struct tab *t)
{
    if (mc_tab(s, t)) return;
    struct doc *d = t->doc;
    size_t a, b;
    sel_range(t, &a, &b);
    if (has_sel(t) && line_of(d, a) != line_of(d, b)) {
        indent_selection(t, 1);
        return;
    }
    size_t line = line_of(d, t->cursor);
    long first = first_char(d, line);
    if (first < 0 || (long)(t->cursor - line_start(d, line)) <= first) {
        indent_selection(t, 1);
        show_cursor(s, t, 0);
        return;
    }
    type_text(s, t, "\t", 1);
}

/* KTextEditor's backspace: the selection; else in the leading space, one
 * indentation level (Indent On Backspace); else one character (a code
 * point, as Backspace Remove Composed Characters is off), or the line
 * break before. */
static void key_backspace(int s, struct tab *t)
{
    if (mc_backspace(s, t)) return;
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    doc_begin(d, t->cursor, t->anchor);
    if (has_sel(t)) {
        remove_selection(t);
    } else {
        size_t line = line_of(d, t->cursor), ls = line_start(d, line);
        if (t->cursor > ls) {
            long first = first_char(d, line);
            size_t col = virtual_column(d, t->cursor), fcol = first < 0 ? 0 : virtual_column(d, ls + (size_t)first);
            if (first < 0 || fcol >= col) {
                change_indent(t, ls, ls, -1);
            } else {
                size_t p = buffer_prev_char(&d->buf, t->cursor);
                doc_remove(d, p, t->cursor - p);
            }
        } else if (line > 0) {
            doc_remove(d, t->cursor - 1, 1);
        }
        t->anchor = t->cursor;
        auto_bracket_backspace(t);
    }
    doc_end(d, t->cursor, t->anchor);
    edited(t);
    show_cursor(s, t, 0);
}

/* Delete: the selection, else the character cluster after the cursor (its
 * grapheme), or the line break. */
static void key_delete(int s, struct tab *t)
{
    if (mc_delete(s, t)) return;
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    doc_begin(d, t->cursor, t->anchor);
    if (has_sel(t)) {
        remove_selection(t);
    } else if (t->cursor < doc_len(d)) {
        size_t le = line_end(d, line_of(d, t->cursor));
        size_t next = t->cursor < le ? min_z(buffer_next_cluster(&d->buf, t->cursor), le) : t->cursor + 1;
        doc_remove(d, t->cursor, next - t->cursor);
    }
    doc_end(d, t->cursor, t->anchor);
    edited(t);
    show_cursor(s, t, 0);
}

static void delete_word(int s, struct tab *t, int right)
{
    if (mc_delete_word(s, t, right)) return;
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    size_t to = right ? word_next(d, t->cursor) : word_prev(d, t->cursor);
    doc_begin(d, t->cursor, t->anchor);
    doc_remove(d, min_z(to, t->cursor), max_z(to, t->cursor) - min_z(to, t->cursor));
    t->anchor = t->cursor;
    doc_end(d, t->cursor, t->anchor);
    edited(t);
    show_cursor(s, t, 0);
}

/* Ctrl+K: the lines of the selection, or the cursor's. */
static void delete_lines(int s, struct tab *t)
{
    if (mc_delete_lines(s, t)) return;
    struct doc *d = t->doc;
    if (!writable(d)) {
        return;
    }
    size_t a, b;
    sel_range(t, &a, &b);
    size_t first = line_of(d, a), last = line_of(d, b);
    if (has_sel(t) && last > first && b == line_start(d, last)) {
        last--;
    }
    size_t from = line_start(d, first), to = last + 1 < d->buf.lines ? line_start(d, last + 1) : line_end(d, last);
    if (last + 1 >= d->buf.lines && first > 0) {
        from = line_end(d, first - 1); /* the last line: the line break before it */
    }
    doc_begin(d, t->cursor, t->anchor);
    doc_remove(d, from, to - from);
    t->anchor = t->cursor;
    doc_end(d, t->cursor, t->anchor);
    edited(t);
    show_cursor(s, t, 0);
}

/* Remove str at line's start, or after its leading space. */
static int remove_from_start(struct doc *d, size_t line, const char *str)
{
    size_t ls = line_start(d, line), n = strlen(str), le = line_end(d, line);
    long first = first_char(d, line);
    size_t at[2] = { ls, first >= 0 ? ls + (size_t)first : ls };
    for (int i = 0; i < 2; i++) {
        if (at[i] + n <= le) {
            char *s = buffer_dup(&d->buf, at[i], n);
            int there = s && memcmp(s, str, n) == 0;
            free(s);
            if (there) {
                doc_remove(d, at[i], n);
                return 1;
            }
        }
    }
    return 0;
}

static int remove_from_end(struct doc *d, size_t line, const char *str)
{
    size_t ls = line_start(d, line), le = line_end(d, line), n = strlen(str);
    size_t last = le;
    while (last > ls && (byte_at(d, last - 1) == ' ' || byte_at(d, last - 1) == '\t')) {
        last--;
    }
    size_t at[2] = { le, last };
    for (int i = 0; i < 2; i++) {
        if (at[i] >= ls + n) {
            char *s = buffer_dup(&d->buf, at[i] - n, n);
            int there = s && memcmp(s, str, n) == 0;
            free(s);
            if (there) {
                doc_remove(d, at[i] - n, n);
                return 1;
            }
        }
    }
    return 0;
}

static int text_at(struct doc *d, size_t at, const char *str)
{
    size_t n = strlen(str);
    if (at + n > doc_len(d)) {
        return 0;
    }
    char *s = buffer_dup(&d->buf, at, n);
    int same = s && memcmp(s, str, n) == 0;
    free(s);
    return same;
}

enum { COMMENT, UNCOMMENT, TOGGLE_COMMENT };

/* KTextEditor's commentSelection, with the marks of the document's
 * language (one language for all of it: tree-sitter's injections are not
 * read). */
static int comment(int s, struct tab *t, int how)
{
    if (mc_comment(s, t, how)) return 1;
    struct doc *d = t->doc;
    const char *mark, *start, *end;
    int after;
    syntax_comment_marks(d->lang, &mark, &after, &start, &end);
    if (!writable(d) || (!mark && !(start && end))) {
        return 0;
    }
    size_t a, b;
    sel_range(t, &a, &b);
    int sel = has_sel(t), done = 0;
    size_t line = line_of(d, t->cursor);
    char m1[16] = "", m2[16] = "";
    if (mark) {
        snprintf(m1, sizeof m1, "%s ", mark);
        snprintf(m2, sizeof m2, "%s", mark);
    }
    doc_begin(d, t->cursor, t->anchor);
    if (how == COMMENT) {
        if (!sel) {
            if (mark) {
                long first = first_char(d, line);
                doc_insert(d, line_start(d, line) + (after && first > 0 ? (size_t)first : 0), m1, strlen(m1));
            } else {
                char st[16], en[16];
                snprintf(st, sizeof st, "%s ", start);
                snprintf(en, sizeof en, " %s", end);
                doc_insert(d, line_start(d, line), st, strlen(st));
                doc_insert(d, line_end(d, line), en, strlen(en));
            }
        } else {
            size_t sl = line_of(d, a), el = line_of(d, b);
            long first = first_char(d, sl);
            if (start && end &&
                (!mark || (long)(a - line_start(d, sl)) > first ||
                 (b > line_start(d, el) && b < line_end(d, el)))) {
                size_t e = b;
                if (e == line_start(d, el) && el > 0) {
                    e = line_end(d, el - 1);
                }
                doc_insert(d, e, end, strlen(end));
                doc_insert(d, a, start, strlen(start));
            } else if (mark) {
                if (b == line_start(d, el) && el > sl) {
                    el--;
                }
                size_t col = 0;
                if (after) {
                    col = (size_t)-1;
                    for (size_t l = el + 1; l-- > sl;) {
                        if (buffer_line_length(&d->buf, l) == 0) {
                            continue;
                        }
                        long f = first_char(d, l);
                        col = min_z(col, f > 0 ? (size_t)f : 0);
                    }
                    col = col == (size_t)-1 ? 0 : col;
                }
                for (size_t l = el + 1; l-- > sl;) {
                    doc_insert(d, line_start(d, l) + col, m1, strlen(m1));
                }
            }
        }
        done = 1;
    } else if (!sel) {
        if (mark) {
            done = remove_from_start(d, line, m1) || remove_from_start(d, line, m2);
        }
        if (!done && start && end) {
            char st[16], en[16];
            snprintf(st, sizeof st, "%s ", start);
            snprintf(en, sizeof en, " %s", end);
            done = remove_from_start(d, line, st) || remove_from_start(d, line, start);
            if (done) {
                if (!remove_from_end(d, line, en)) {
                    remove_from_end(d, line, end);
                }
            }
        }
    } else {
        size_t sl = line_of(d, a), el = line_of(d, b);
        /* The start and end marks around the selection, inside its spaces. */
        if (start && end) {
            size_t p = a, q = b;
            while (p < q && (byte_at(d, p) == ' ' || byte_at(d, p) == '\t' || byte_at(d, p) == '\n')) {
                p++;
            }
            while (q > p && (byte_at(d, q - 1) == ' ' || byte_at(d, q - 1) == '\t' || byte_at(d, q - 1) == '\n')) {
                q--;
            }
            if (q >= p + strlen(start) + strlen(end) && text_at(d, p, start) && text_at(d, q - strlen(end), end)) {
                doc_remove(d, q - strlen(end), strlen(end));
                doc_remove(d, p, strlen(start));
                done = 1;
            }
        }
        if (!done && mark) {
            if (b == line_start(d, el) && el > sl) {
                el--;
            }
            int all = 1;
            for (size_t l = sl; how == TOGGLE_COMMENT && l <= el && all; l++) {
                long f = first_char(d, l);
                if (buffer_line_length(&d->buf, l) == 0) {
                    continue;
                }
                all = f >= 0 && text_at(d, line_start(d, l) + (size_t)f, m2);
            }
            if (all) {
                for (size_t l = el + 1; l-- > sl;) {
                    done = remove_from_start(d, l, m1) || remove_from_start(d, l, m2) || done;
                }
            }
        }
    }
    doc_end(d, t->cursor, t->anchor);
    if (!done && how == TOGGLE_COMMENT) {
        return comment(s, t, COMMENT);
    }
    edited(t);
    show_cursor(s, t, 0);
    return done;
}

static void copy(struct tab *t, int cut)
{
    if (mc_copy(t, cut, 0)) return;
    struct doc *d = t->doc;
    size_t a, b;
    sel_range(t, &a, &b);
    int line = !has_sel(t); /* Smart Copy Cut: the line */
    if (line) {
        size_t l = line_of(d, t->cursor);
        a = line_start(d, l);
        b = l + 1 < d->buf.lines ? line_start(d, l + 1) : line_end(d, l);
    }
    char *text = buffer_dup(&d->buf, a, b - a);
    if (!text) {
        return;
    }
    size_t n = b - a;
    if (line && (n == 0 || text[n - 1] != '\n')) {
        char *more = realloc(text, n + 2);
        if (more) {
            text = more;
            text[n++] = '\n';
        }
    }
    clipboard_set(0, text, n);
    free(text);
    if (cut && writable(d)) {
        doc_begin(d, t->cursor, t->anchor);
        doc_remove(d, a, b - a);
        t->anchor = t->cursor;
        doc_end(d, t->cursor, t->anchor);
        edited(t);
    } else if (line && !cut) {
        t->cursor = t->anchor = a; /* the cursor to the line's start, as Kate's copy */
    }
}

/* KTextEditor's paste: line ends made \n, a step of its own in undo. */
static void paste_text(int s, struct tab *t, char *text, size_t n)
{
    struct doc *d = t->doc;
    if (!writable(d) || n == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r') {
            text[o++] = '\n';
            i += i + 1 < n && text[i + 1] == '\n';
        } else if ((unsigned char)text[i] == 0xE2 && i + 2 < n && (unsigned char)text[i + 1] == 0x80 &&
                   ((unsigned char)text[i + 2] == 0xA8 || (unsigned char)text[i + 2] == 0xA9)) {
            text[o++] = '\n'; /* U+2028, U+2029 */
            i += 2;
        } else {
            text[o++] = text[i];
        }
    }
    if (mc_paste(s, t, text, o)) return;
    doc_safe_point(d);
    doc_begin(d, t->cursor, t->anchor);
    if (has_sel(t)) {
        remove_selection(t);
    }
    doc_insert(d, t->cursor, text, o);
    t->anchor = t->cursor;
    doc_end(d, t->cursor, t->anchor);
    doc_safe_point(d);
    edited(t);
    show_cursor(s, t, 0);
}

static void say_toast(const char *msg);

static void paste(int s, struct tab *t, int primary)
{
    int fd;
    size_t size;
    int status = clipboard_get(primary, &fd, &size);
    if (status != CLIPBOARD_OK) {
        if (status != CLIPBOARD_EMPTY) {
            say_toast(clipboard_error(status));
        }
        return;
    }
    char *text = malloc(size + 1);
    size_t got = 0;
    while (text && got < size) {
        ssize_t n = pread(fd, text + got, size - got, (off_t)got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);
    if (text && got == size) {
        paste_text(s, t, text, size);
    }
    free(text);
}

static void undo_redo(int s, struct tab *t, int redo)
{
    if (mc_undo_redo(s, t, redo)) return;
    size_t c, a;
    if ((redo ? doc_redo : doc_undo)(t->doc, &c, &a)) {
        t->cursor = c;
        t->anchor = a;
        edited(t);
        show_cursor(s, t, 0);
    }
}

/* The bracket at at and the one that matches it (of the same colour). */
static int match_bracket(struct doc *d, size_t at, size_t *other)
{
    int c = byte_at(d, at);
    const char *open = "([{", *close = ")]}";
    const char *o = c > 0 ? strchr(open, c) : NULL, *k = c > 0 ? strchr(close, c) : NULL;
    if (!c || (!o && !k)) {
        return 0;
    }
    int forward = o != NULL, want = forward ? close[o - open] : open[k - close], depth = 0;
    size_t line = line_of(d, at), lo = line > BRACKET_LINES ? line_start(d, line - BRACKET_LINES) : 0;
    size_t hi = line + BRACKET_LINES < d->buf.lines ? line_end(d, line + BRACKET_LINES) : doc_len(d);
    /* The colours of the range, to skip brackets in strings and comments. */
    size_t from = forward ? at : lo, to = forward ? hi : at + 1;
    uint32_t *colour = d->syntax ? colours_of(d, from, to) : NULL;
    uint32_t mine = colour ? colour[at - from] : 0;
    for (size_t p = at;;) {
        int b = byte_at(d, p);
        if (!colour || colour[p - from] == mine) {
            if (b == c) {
                depth++;
            } else if (b == want && --depth == 0) {
                *other = p;
                free(colour);
                return 1;
            }
        }
        if (forward) {
            if (++p >= hi) {
                break;
            }
        } else {
            if (p == lo) {
                break;
            }
            p--;
        }
    }
    free(colour);
    return 0;
}

static int bracket_pair(const struct tab *t, size_t *a, size_t *b)
{
    struct doc *d = t->doc;
    size_t le = line_end(d, line_of(d, t->cursor)), other;
    if (t->cursor < le && match_bracket(d, t->cursor, &other)) {
        *a = t->cursor;
        *b = other;
        return 1;
    }
    if (!d->overwrite && t->cursor > line_start(d, line_of(d, t->cursor)) && match_bracket(d, t->cursor - 1, &other)) {
        *a = t->cursor - 1;
        *b = other;
        return 1;
    }
    return 0;
}

/* Ctrl+6, Ctrl+Shift+6 (KateViewInternal::cursorToMatchingBracket): to
 * the other bracket (after it, when it closes), selecting with sel. */
static void to_bracket(int s, struct tab *t, int sel)
{
    if (mc_to_bracket(s, t, sel)) return;
    size_t a, b;
    if (!bracket_pair(t, &a, &b)) {
        return;
    }
    size_t to = b > a ? b + 1 : b;
    if (sel && b < a && a + 1 == t->cursor) {
        to = b;
    }
    move_to(s, t, to, sel, 0);
}

/* Up and Down by visual rows, keeping the column (m_preservedX); past the
 * first row to the line's start, past the last to its end. */
static void move_vertical(int s, struct tab *t, long rows, int sel)
{
    int width = text_width(s, t), row, x;
    place_of(t, width, t->cursor, &row, &x);
    if (t->goal < 0) {
        t->goal = x;
    }
    size_t line = line_of(t->doc, t->cursor);
    int r = row;
    move_rows(t, width, &line, &r, rows);
    size_t to;
    if (line == line_of(t->doc, t->cursor) && r == row) {
        /* At the last visible line, its end: a fold reaching the end stays closed (cursorDown). */
        to = rows < 0 ? line_start(t->doc, 0) : line_end(t->doc, line_of(t->doc, t->cursor));
        if (rows < 0 && line != 0) {
            to = t->cursor;
        }
    } else {
        to = at_column(t, width, line, r, t->goal, 0);
    }
    move_to(s, t, to, sel, 1);
}

/* PageUp and PageDown (PageUp/PageDown Moves Cursor is off): the view by a
 * page less a row, the cursor at the same place in it; at the top or the
 * bottom of the text, the cursor a page. */
static void page(int s, struct tab *t, int dir, int sel)
{
    int y, h, width = text_width(s, t), row, x;
    text_rows(s, &y, &h);
    place_of(t, width, t->cursor, &row, &x);
    if (t->goal < 0) {
        t->goal = x;
    }
    size_t tl = line_of(t->doc, t->top), before_top = t->top;
    int before_sub = t->sub;
    /* The cursor's row in the view. */
    size_t cl = line_of(t->doc, t->cursor), line = tl;
    int r = t->sub;
    long view_row = 0;
    while (view_row < h && (line < cl || (line == cl && r < row))) {
        size_t pl = line;
        int pr = r;
        move_rows(t, width, &line, &r, 1);
        if (pl == line && pr == r) {
            break;
        }
        view_row++;
    }
    line = tl;
    r = t->sub;
    move_rows(t, width, &line, &r, dir * (h - 1 > 0 ? h - 1 : 1));
    t->top = line_start(t->doc, line);
    t->sub = r;
    clamp_top(s, t);
    if (t->top == before_top && t->sub == before_sub) {
        move_vertical(s, t, dir * (h - 1 > 0 ? h - 1 : 1), sel);
        return;
    }
    line = line_of(t->doc, t->top);
    r = t->sub;
    move_rows(t, width, &line, &r, view_row);
    size_t to = at_column(t, width, line, r, t->goal, 0);
    t->cursor = to;
    if (!sel) {
        t->anchor = to;
    } else {
        primary_pending = 1;
        primary_since = now();
    }
    doc_safe_point(t->doc);
}

/* Home (Smart Home): a continuation row's start, else the first character
 * that is not a space, else (there already) the line's start. */
static void key_home(int s, struct tab *t, int sel)
{
    struct doc *d = t->doc;
    int width = text_width(s, t), shift;
    size_t line = line_of(d, t->cursor), ls = line_start(d, line);
    int n = layout(t, width, line, row_buf, MAX_ROWS, &shift), r = row_of(row_buf, n, t->cursor);
    if (r > 0 && t->cursor != row_buf[r]) {
        move_to(s, t, row_buf[r], sel, 0);
        return;
    }
    long first = first_char(d, line);
    size_t to = first < 0 || ls + (size_t)first == t->cursor ? ls : ls + (size_t)first;
    move_to(s, t, to, sel, 0);
}

/* End: a wrapped row's end, then the line's; there already (Smart End),
 * after its last character that is not a space. */
static void key_end(int s, struct tab *t, int sel)
{
    struct doc *d = t->doc;
    int width = text_width(s, t), shift;
    size_t line = line_of(d, t->cursor), le = line_end(d, line);
    int n = layout(t, width, line, row_buf, MAX_ROWS, &shift), r = row_of(row_buf, n, t->cursor);
    if (r + 1 < n) {
        size_t end = buffer_prev_cluster(&d->buf, row_buf[r + 1]);
        if (t->cursor != end) {
            move_to(s, t, end, sel, 0);
            return;
        }
    }
    size_t to = le;
    if (t->cursor == le) {
        while (to > line_start(d, line) && (byte_at(d, to - 1) == ' ' || byte_at(d, to - 1) == '\t')) {
            to--;
        }
    }
    move_to(s, t, to, sel, 0);
}

/* Left and Right: a character cluster, over line ends. */
static void key_left_right(int s, struct tab *t, int right, int sel)
{
    struct doc *d = t->doc;
    size_t to = right ? buffer_next_cluster(&d->buf, t->cursor) : buffer_prev_cluster(&d->buf, t->cursor);
    move_to(s, t, to, sel, 0); /* over a fold it unfolds, as WrappingCursor and ensureLineIsVisible */
}

/* Alt+Home, Alt+End: the first or last row shown. */
static void view_edge(int s, struct tab *t, int bottom, int sel)
{
    int y, h, width = text_width(s, t);
    text_rows(s, &y, &h);
    size_t line = line_of(t->doc, t->top);
    int r = t->sub;
    if (bottom) {
        move_rows(t, width, &line, &r, h - 1);
    }
    if (t->goal < 0) {
        int row, x;
        place_of(t, width, t->cursor, &row, &x);
        t->goal = x;
    }
    move_to(s, t, at_column(t, width, line, r, t->goal, 0), sel, 1);
}

/* Ctrl+Up, Ctrl+Down, the wheel: the view only. */
static void scroll_rows(int s, struct tab *t, long rows)
{
    size_t line = line_of(t->doc, t->top);
    int r = t->sub;
    move_rows(t, text_width(s, t), &line, &r, rows);
    t->top = line_start(t->doc, line);
    t->sub = r;
    clamp_top(s, t);
}

/*
 * The bars (KateSearchBar, KateGotoBar).
 */

static void field_set(struct field *f, const char *text, size_t n)
{
    n = n < sizeof f->s - 1 ? n : sizeof f->s - 1;
    memcpy(f->s, text, n);
    f->s[n] = '\0';
    f->len = f->cur = (int)n;
}

static void field_insert(struct field *f, const char *text, size_t n)
{
    if (f->all) {
        f->len = f->cur = 0;
        f->s[0] = '\0';
        f->all = 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n' || text[i] == '\r') {
            n = i; /* one line */
        }
    }
    if ((size_t)f->len + n >= sizeof f->s) {
        return;
    }
    memmove(f->s + f->cur + n, f->s + f->cur, (size_t)(f->len - f->cur) + 1);
    memcpy(f->s + f->cur, text, n);
    f->len += (int)n;
    f->cur += (int)n;
}

static int field_prev(const struct field *f, int at)
{
    if (at <= 0) {
        return 0;
    }
    int p = at - 1;
    while (p > 0 && ((unsigned char)f->s[p] & 0xC0) == 0x80) {
        p--;
    }
    return p;
}

static int field_next(const struct field *f, int at)
{
    size_t k;
    if (at >= f->len) {
        return f->len;
    }
    text_decode(f->s + at, (size_t)(f->len - at), &k);
    return at + (int)k;
}

static void field_cut(struct field *f, int a, int b)
{
    memmove(f->s + a, f->s + b, (size_t)(f->len - b) + 1);
    f->len -= b - a;
    f->cur = a;
}

/* A key for a line edit: 1 if it took it (and *changed when its text did). */
static int field_key(struct field *f, int key, int mods, int *changed)
{
    *changed = 0;
    const char *typed = keys_typed(key);
    if (typed && !(mods & (KEYS_CTRL | KEYS_ALT))) {
        field_insert(f, typed, strlen(typed));
        *changed = 1;
        return 1;
    }
    if (mods & KEYS_ALT) {
        return 0;
    }
    if (key == '\b' && !(mods & KEYS_CTRL)) {
        if (f->all) {
            field_cut(f, 0, f->len);
            f->all = 0;
        } else if (f->cur > 0) {
            field_cut(f, field_prev(f, f->cur), f->cur);
        }
        *changed = 1;
        return 1;
    }
    if (key == K_DEL && !(mods & KEYS_SHIFT)) {
        if (f->all) {
            field_cut(f, 0, f->len);
            f->all = 0;
        } else if (f->cur < f->len) {
            field_cut(f, f->cur, field_next(f, f->cur));
        }
        *changed = 1;
        return 1;
    }
    if (key == K_LEFT || key == K_RIGHT || key == K_BOL || key == K_EOL) {
        f->cur = key == K_BOL ? 0 : key == K_EOL ? f->len : key == K_LEFT ? field_prev(f, f->cur) : field_next(f, f->cur);
        f->all = 0;
        return 1;
    }
    if ((mods & KEYS_CTRL) && key == 'a') {
        f->all = 1;
        f->cur = f->len;
        return 1;
    }
    return 0;
}

static int bar_settings(void)
{
    return bar_kind == BAR_FIND || bar_kind == BAR_POWER ? bar_kind : bar.last;
}

static int search_mode(void)
{
    return bar_settings() == BAR_POWER ? bar.mode : SEARCH_PLAIN;
}

static int search_case(void)
{
    return bar_settings() == BAR_POWER ? bar.power_case : bar.inc_case;
}

static int pattern_ok(void)
{
    return search_valid(bar.find.s, search_mode());
}

/* The pattern compiled for the bar's settings: 0, or -1 (MATCH_INVALID). */
static int compile_pattern(void)
{
    if (!bar.find.s[0]) {
        search_free(&bar.s);
        return -1;
    }
    if (search_compile(&bar.s, bar.find.s, search_mode(), search_case()) < 0) {
        snprintf(bar.error, sizeof bar.error, _("Invalid regular expression: %.120s"), bar.s.error);
        bar.match = MATCH_INVALID;
        return -1;
    }
    return 0;
}

static void clear_highlights(struct doc *d)
{
    struct docui *u = ui_of(d);
    free(u->hl);
    u->hl = NULL;
    u->nhl = 0;
}

static void close_bar(void)
{
    struct tab *t = cur_tab();
    if (t) {
        clear_highlights(t->doc);
    }
    if (bar_kind == BAR_FIND || bar_kind == BAR_POWER) {
        bar.last = bar_kind;
    }
    bar_kind = BAR_NONE;
    bar.focus = FOCUS_VIEW;
}

/* The match selected, the cursor at its end (selectRange). */
static void select_match(int s, struct tab *t, size_t a, size_t b)
{
    t->anchor = a;
    t->cursor = b;
    t->goal = -1;
    doc_safe_point(t->doc);
    show_cursor(s, t, 1);
}

/* onIncPatternChanged: from where the search started, else from the top. */
static void inc_search(int s)
{
    struct tab *t = tab_of(s);
    if (!t) {
        return;
    }
    clear_highlights(t->doc);
    if (!bar.find.s[0]) {
        t->cursor = t->anchor = min_z(bar.inc_start, doc_len(t->doc));
        bar.match = MATCH_NOTHING;
        return;
    }
    if (compile_pattern() < 0) {
        return;
    }
    size_t a, b, len = doc_len(t->doc);
    int found = search_find(&bar.s, &t->doc->buf, min_z(bar.inc_start, len), 0, 0, len, &a, &b), wrapped = 0;
    if (!found) {
        found = search_find(&bar.s, &t->doc->buf, 0, 0, 0, len, &a, &b);
        wrapped = found;
    }
    if (found) {
        select_match(s, t, a, b);
        bar.match = wrapped ? MATCH_WRAPPED_BOTTOM : MATCH_FOUND;
    } else {
        bar.match = MATCH_MISMATCH;
    }
}

/* The search's range for the selection only (Search in the selection only). */
static void search_range(struct tab *t, size_t *lo, size_t *hi)
{
    *lo = 0;
    *hi = doc_len(t->doc);
    if (bar_settings() == BAR_POWER && bar.selection_only) {
        struct docui *u = ui_of(t->doc);
        *lo = u->scope_valid ? min_z(u->scope_lo, *hi) : 0;
        *hi = u->scope_valid ? min_z(u->scope_hi, *hi) : 0;
    }
}

/* findOrReplace: the next match from the cursor (past a selected match, which
 * is replaced first when replacing), wrapping around to the other end. */
static void find_or_replace(int s, int backwards, const char *replacement)
{
    struct tab *t = tab_of(s);
    if (!t || compile_pattern() < 0) {
        return;
    }
    struct doc *d = t->doc;
    clear_highlights(d);
    size_t lo, hi, a, b, sa, sb;
    search_range(t, &lo, &hi);
    int sel = has_sel(t), only = bar_settings() == BAR_POWER && bar.selection_only;
    if (only && !ui_of(d)->scope_valid) {
        bar.match = MATCH_MISMATCH;
        return;
    }
    sel_range(t, &sa, &sb);
    size_t from = backwards ? (sel ? sb : t->cursor) : (sel ? sa : t->cursor);
    int found = search_find(&bar.s, &d->buf, from, backwards, lo, hi, &a, &b);
    size_t after_a = 0, after_b = 0;
    int replaced = 0;
    if (found && sel && a == sa && b == sb) {
        if (replacement && writable(d)) {
            size_t n;
            char *text = search_replacement(&bar.s, &d->buf, replacement, ++bar.counter, &n);
            if (text) {
                doc_begin(d, t->cursor, t->anchor);
                doc_remove(d, a, b - a);
                doc_insert(d, a, text, n);
                t->cursor = t->anchor = a + n;
                doc_end(d, t->cursor, t->anchor);
                doc_safe_point(d);
                free(text);
                after_a = a;
                after_b = a + n;
                replaced = 1;
                hi = hi - (b - a) + n;
                if (only) {
                    search_range(t, &lo, &hi);
                }
            }
        }
        from = backwards ? (replaced ? after_a : sa) : (replaced ? after_b : sb);
        found = backwards ? search_find(&bar.s, &d->buf, from, 1, lo, from, &a, &b) :
                            search_find(&bar.s, &d->buf, from, 0, lo, hi, &a, &b);
    } else if (found && a == b && b == t->cursor) {
        /* An empty match where the cursor is: one character on. */
        from = backwards ? buffer_prev_char(&d->buf, a) : buffer_next_char(&d->buf, a);
        found = from != a && search_find(&bar.s, &d->buf, from, backwards, lo, hi, &a, &b);
    }
    int wrapped = 0;
    if (!found && !(replaced && only)) {
        wrapped = 1;
        post(d, NOTE_INFO, 0, 2.0, "%s", _("Search wrapped")); /* showSearchWrappedHint */
        found = search_find(&bar.s, &d->buf, backwards ? hi : lo, backwards, lo, hi, &a, &b);
    }
    if (found) {
        select_match(s, t, a, b);
    }
    bar.match = !found ? MATCH_MISMATCH : !wrapped ? MATCH_FOUND : backwards ? MATCH_WRAPPED_TOP : MATCH_WRAPPED_BOTTOM;
    bar.inc_start = t->cursor;
    if (replaced) {
        struct docui *u = ui_of(d);
        u->hl = malloc(2 * sizeof *u->hl);
        if (u->hl) {
            u->hl[0] = after_a;
            u->hl[1] = after_b;
            u->nhl = 1;
            u->hl_replace = 1;
        }
    }
}

/* Every match in range [lo, hi), for Find All and Replace All. */
static size_t all_matches(struct doc *d, size_t lo, size_t hi, size_t **out)
{
    size_t n = 0, size = 0, *m = NULL;
    for (size_t at = lo; at <= hi;) {
        size_t a, b;
        if (!search_find(&bar.s, &d->buf, at, 0, lo, hi, &a, &b)) {
            break;
        }
        if (n == size) {
            size = size ? size * 2 : 256;
            size_t *more = realloc(m, 2 * size * sizeof *m);
            if (!more) {
                break;
            }
            m = more;
        }
        m[2 * n] = a;
        m[2 * n + 1] = b;
        n++;
        at = b > a ? b : buffer_next_char(&d->buf, b);
        if (b == a && at == b) {
            break;
        }
    }
    *out = m;
    return n;
}

static void find_all(int s)
{
    struct tab *t = tab_of(s);
    if (!t || compile_pattern() < 0) {
        return;
    }
    struct doc *d = t->doc;
    clear_highlights(d);
    size_t lo, hi, *m;
    search_range(t, &lo, &hi);
    if (bar_settings() == BAR_POWER && bar.selection_only && !ui_of(d)->scope_valid) {
        bar.match = MATCH_MISMATCH;
        return;
    }
    size_t n = all_matches(d, lo, hi, &m);
    struct docui *u = ui_of(d);
    u->hl = m;
    u->nhl = n < MAX_HIGHLIGHT ? n : MAX_HIGHLIGHT;
    u->hl_replace = 0;
    post(d, NOTE_POSITIVE, 0, 3.0, P_("%zu match found", "%zu matches found", n), n);
    bar.match = n ? MATCH_FOUND : MATCH_MISMATCH;
}

/* Replace All: one undo step, the replacements highlighted, their count. */
static size_t replace_all(int s)
{
    struct tab *t = tab_of(s);
    if (!t || compile_pattern() < 0 || !writable(t->doc)) {
        return 0;
    }
    struct doc *d = t->doc;
    clear_highlights(d);
    size_t lo, hi, *m;
    search_range(t, &lo, &hi);
    if (bar_settings() == BAR_POWER && bar.selection_only && !ui_of(d)->scope_valid) {
        bar.match = MATCH_MISMATCH;
        return 0;
    }
    size_t n = all_matches(d, lo, hi, &m);
    struct replacement *r = n ? calloc(n, sizeof *r) : NULL;
    size_t done = 0;
    for (size_t i = 0; r && i < n; i++) {
        size_t a, b;
        /* The match again, for its groups. */
        if (!search_find(&bar.s, &d->buf, m[2 * i], 0, m[2 * i], hi, &a, &b) || a != m[2 * i]) {
            continue;
        }
        size_t len;
        char *text = search_replacement(&bar.s, &d->buf, bar.replace.s, (int)i + 1, &len);
        if (!text) {
            break;
        }
        r[done++] = (struct replacement){ a, b - a, text, len };
    }
    if (done) {
        size_t *hl = malloc(2 * done * sizeof *hl);
        doc_begin(d, t->cursor, t->anchor);
        if (doc_replace_many(d, r, done) == 0 && hl) {
            for (size_t i = 0; i < done; i++) {
                hl[2 * i] = r[i].at; /* now the replacements' places */
                hl[2 * i + 1] = r[i].at + r[i].len;
            }
        }
        t->anchor = t->cursor;
        doc_end(d, t->cursor, t->anchor);
        doc_safe_point(d); /* never merged with what follows */
        edited(t);
        clear_highlights(d);
        struct docui *u = ui_of(d);
        u->hl = hl;
        u->nhl = hl ? (done < MAX_HIGHLIGHT ? done : MAX_HIGHLIGHT) : 0;
        u->hl_replace = 1;
    } else {
        for (size_t i = 0; r && i < n; i++) {
            free((char *)r[i].text);
        }
    }
    free(r);
    free(m);
    post(d, NOTE_POSITIVE, 0, 3.0, P_("%zu replacement made", "%zu replacements made", done), done);
    bar.match = done ? MATCH_FOUND : MATCH_MISMATCH;
    return done;
}

/* Ctrl+F (enterIncrementalMode) and Ctrl+R (enterPowerMode): the pattern
 * from a selection on one line, else the bar's, else the word at the
 * cursor; all of it selected in its field. */
static void open_search(int power)
{
    struct tab *t = cur_tab();
    if (!t) {
        return;
    }
    struct doc *d = t->doc;
    size_t a, b;
    sel_range(t, &a, &b);
    int was = bar_kind;
    if (has_sel(t) && line_of(d, a) == line_of(d, b)) {
        char *text = buffer_dup(&d->buf, a, b - a);
        if (text) {
            field_set(&bar.find, text, b - a);
            free(text);
        }
    } else if (!(was == BAR_FIND || was == BAR_POWER) || !bar.find.s[0]) {
        size_t wa, wb;
        word_at(d, t->cursor, &wa, &wb);
        char *text = buffer_dup(&d->buf, wa, wb - wa);
        if (text) {
            field_set(&bar.find, text, wb - wa);
            free(text);
        }
    }
    bar_kind = power ? BAR_POWER : BAR_FIND;
    bar.focus = FOCUS_BAR;
    bar.item = ITEM_FIND;
    bar.find.all = 1;
    bar.find.cur = bar.find.len;
    bar.match = MATCH_NOTHING;
    bar.inc_start = has_sel(t) ? a : t->cursor;
    if (power) {
        struct docui *u = ui_of(d);
        u->scope_valid = has_sel(t);
        u->scope_lo = a;
        u->scope_hi = b;
        bar.selection_only = has_sel(t) && line_of(d, a) != line_of(d, b);
    } else if (bar.find.s[0]) {
        inc_search(active);
    }
}

static void open_goto(void)
{
    struct tab *t = cur_tab();
    if (!t) {
        return;
    }
    char num[32];
    int n = snprintf(num, sizeof num, "%zu", line_of(t->doc, t->cursor) + 1);
    field_set(&bar.line, num, (size_t)n);
    bar.line.all = 1;
    bar_kind = BAR_GOTO;
    bar.focus = FOCUS_BAR;
    bar.item = ITEM_LINE;
}

/* KateGotoBar::gotoLine: N is line N, 0 the first, -N the Nth from the end;
 * the cursor at its start. */
static void go_to_line(int s)
{
    struct tab *t = tab_of(s);
    if (!t) {
        return;
    }
    long lines = (long)t->doc->buf.lines, v = strtol(bar.line.s, NULL, 10);
    v = v < -lines ? -lines : v > lines ? lines : v;
    if (v < 0) {
        v += lines;
    } else if (v > 0) {
        v--;
    }
    close_bar();
    t->cursor = t->anchor = line_start(t->doc, (size_t)v);
    t->goal = -1;
    doc_safe_point(t->doc);
    show_cursor(s, t, 1);
}

/* Ctrl+H, Ctrl+Shift+H (findSelectedForwards): the word at the cursor if
 * nothing is selected, else the next match of the selection, its case. */
static void find_selected(int s, int backwards)
{
    struct tab *t = tab_of(s);
    if (!t) {
        return;
    }
    struct doc *d = t->doc;
    if (!has_sel(t)) {
        size_t a, b;
        word_at(d, t->cursor, &a, &b);
        t->anchor = a;
        t->cursor = b;
        return;
    }
    size_t a, b;
    sel_range(t, &a, &b);
    char *text = buffer_dup(&d->buf, a, b - a);
    if (!text) {
        return;
    }
    struct search one = { 0 };
    if (search_compile(&one, text, SEARCH_PLAIN, 1) == 0) {
        size_t ma, mb, len = doc_len(d);
        int found = backwards ? (a > 0 && search_find(&one, &d->buf, a, 1, 0, a, &ma, &mb)) :
                                search_find(&one, &d->buf, b, 0, 0, len, &ma, &mb);
        if (!found) {
            found = search_find(&one, &d->buf, backwards ? len : 0, backwards, 0, len, &ma, &mb);
        }
        if (found) {
            select_match(s, t, ma, mb);
        }
    }
    search_free(&one);
    free(text);
}

static const int find_ring[] = { ITEM_FIND, ITEM_NEXT, ITEM_PREV, ITEM_CASE, ITEM_POWER };
static const int power_ring[] = { ITEM_FIND, ITEM_NEXT, ITEM_PREV, ITEM_MODE, ITEM_CASE, ITEM_SELECTION,
                                  ITEM_REPLACE, ITEM_REPLACE_ONE, ITEM_REPLACE_ALL, ITEM_FIND_ALL };
static const int goto_ring[] = { ITEM_LINE, ITEM_GO };

static void cycle_focus(int dir)
{
    const int *ring = bar_kind == BAR_FIND ? find_ring : bar_kind == BAR_POWER ? power_ring : goto_ring;
    int n = bar_kind == BAR_FIND ? 5 : bar_kind == BAR_POWER ? 10 : 2, at = 0;
    for (int i = 0; i < n; i++) {
        if (ring[i] == bar.item) {
            at = i;
        }
    }
    bar.item = ring[(at + dir + n) % n];
}

static void set_mode(int mode)
{
    bar.mode = mode;
    if (mode == SEARCH_REGEX) {
        bar.power_case = 1; /* Kate: a regular expression matches case */
    }
    bar.match = MATCH_NOTHING;
}

/* A bar's item chosen (Enter, Space, a click). */
static void bar_activate(int item, int mods)
{
    switch (item) {
    case ITEM_FIND:
        find_or_replace(active, (mods & KEYS_SHIFT) != 0, NULL);
        if (mods & KEYS_CTRL) {
            close_bar();
        }
        break;
    case ITEM_NEXT:
    case ITEM_PREV:
        if (pattern_ok()) {
            find_or_replace(active, item == ITEM_PREV, NULL);
        }
        break;
    case ITEM_REPLACE:
    case ITEM_REPLACE_ONE:
        if (pattern_ok()) {
            find_or_replace(active, 0, bar.replace.s);
        }
        break;
    case ITEM_REPLACE_ALL:
        if (pattern_ok()) {
            replace_all(active);
        }
        break;
    case ITEM_FIND_ALL:
        if (pattern_ok()) {
            find_all(active);
        }
        break;
    case ITEM_MODE:
        set_mode((bar.mode + 1) % 4);
        break;
    case ITEM_CASE:
        if (bar_kind == BAR_FIND) {
            bar.inc_case = !bar.inc_case;
            inc_search(active); /* onMatchCaseToggled */
        } else {
            bar.power_case = !bar.power_case;
            bar.match = MATCH_NOTHING;
        }
        break;
    case ITEM_SELECTION:
        bar.selection_only = !bar.selection_only;
        if (bar.selection_only) {
            struct tab *t = cur_tab();
            if (t) {
                struct docui *u = ui_of(t->doc);
                u->scope_valid = has_sel(t);
                sel_range(t, &u->scope_lo, &u->scope_hi);
            }
        }
        break;
    case ITEM_POWER:
        open_search(1);
        break;
    case ITEM_LINE:
    case ITEM_GO:
        go_to_line(active);
        break;
    }
}

/* A key while a bar has the focus: 1 if the bar took it. */
static int bar_key(int key, int mods)
{
    struct field *f = bar.item == ITEM_FIND ? &bar.find : bar.item == ITEM_REPLACE ? &bar.replace :
                      bar.item == ITEM_LINE ? &bar.line : NULL;
    if (key == 27 && !(mods & (KEYS_CTRL | KEYS_ALT))) {
        close_bar();
        return 1;
    }
    if (key == '\t' && !(mods & (KEYS_CTRL | KEYS_ALT))) {
        cycle_focus(mods & KEYS_SHIFT ? -1 : 1);
        return 1;
    }
    if (mods == KEYS_ALT && key < 0x80 && key > 0) {
        int k = tolower(key);
        if (bar_kind == BAR_POWER) {
            switch (k) {
            case 'd': bar.item = ITEM_FIND; return 1;
            case 'l': bar.item = ITEM_REPLACE; return 1;
            case 'm': set_mode((bar.mode + 1) % 4); bar.item = ITEM_MODE; return 1;
            case 'r': bar_activate(ITEM_REPLACE_ONE, 0); return 1;
            case 'a': bar_activate(ITEM_REPLACE_ALL, 0); return 1;
            case 'f': bar_activate(ITEM_FIND_ALL, 0); return 1;
            }
        } else if (bar_kind == BAR_FIND && k == 'i') {
            bar.item = ITEM_FIND;
            return 1;
        }
    }
    if (key == '\n') {
        bar_activate(bar.item, mods);
        return 1;
    }
    if (f) {
        int changed;
        if (bar.item == ITEM_LINE && keys_typed(key) && !(mods & (KEYS_CTRL | KEYS_ALT)) &&
            !isdigit(key) && key != '-') {
            return 1; /* a spin box: numbers only */
        }
        if ((mods & KEYS_CTRL) && key == 'v') {
            int fd;
            size_t size;
            if (clipboard_get(0, &fd, &size) == CLIPBOARD_OK) {
                char buf[1024];
                ssize_t n = pread(fd, buf, size < sizeof buf ? size : sizeof buf, 0);
                close(fd);
                if (n > 0) {
                    field_insert(f, buf, (size_t)n);
                }
                if (bar.item == ITEM_FIND && bar_kind == BAR_FIND) {
                    inc_search(active);
                }
            }
            return 1;
        }
        if (field_key(f, key, mods, &changed)) {
            if (changed && bar.item == ITEM_FIND) {
                if (bar_kind == BAR_FIND) {
                    inc_search(active);
                } else {
                    bar.match = MATCH_NOTHING; /* onPowerPatternChanged */
                }
            }
            return 1;
        }
        return 0;
    }
    if (key == ' ' && !(mods & (KEYS_CTRL | KEYS_ALT))) {
        bar_activate(bar.item, 0);
        return 1;
    }
    if (bar.item == ITEM_MODE && (key == K_UP || key == K_DOWN)) {
        set_mode((bar.mode + (key == K_DOWN ? 1 : 3)) % 4);
        return 1;
    }
    return 0;
}

/* F3, Shift+F3: with the bar's pattern, or the incremental bar opened. */
static void find_next(int backwards)
{
    if (!bar.find.s[0]) {
        open_search(0);
        return;
    }
    find_or_replace(active, backwards, NULL);
}

/*
 * Dialogs (KMessageBox's, over the picture until answered) and a line
 * typed for a file's name.
 */

static void say_toast(const char *msg)
{
    snprintf(toast, sizeof toast, "%s", msg);
    toast_until = now() + 0.8;
}

/* A question: title, text (\n breaks lines), buttons (& before a key);
 * the index chosen, or cancel (Esc, a click outside). def has the focus. */
static int ask(const char *title, const char *text, const char *const *buttons, int n, int def, int cancel)
{
    int focus = def;
    for (;;) {
        draw();
        ui_hits_clear();
        int cols = screen_cols(), rows = screen_rows(), w = cols * 3 / 5 > 50 ? cols * 3 / 5 : cols - 2;
        w = w > cols - 2 ? cols - 2 : w;
        /* The text's rows, word-wrapped. */
        char lines[24][256];
        int nl = 0;
        for (const char *p = text; *p && nl < 24;) {
            size_t len = strcspn(p, "\n");
            const char *e = p + len;
            while (p <= e && nl < 24) {
                size_t take = 0, last_space = 0;
                int cells = 0;
                while (p + take < e) {
                    size_t k;
                    uint32_t cp = text_decode(p + take, (size_t)(e - p - (long)take), &k);
                    if (cells + screen_char_cells(cp) > w - 4) {
                        break;
                    }
                    cells += screen_char_cells(cp);
                    take += k;
                    if (cp == ' ') {
                        last_space = take;
                    }
                }
                if (p + take < e && last_space) {
                    take = last_space;
                }
                snprintf(lines[nl++], sizeof lines[0], "%.*s", (int)take, p);
                p += take;
                if (p >= e) {
                    break;
                }
            }
            p = *e ? e + 1 : e;
            if (!*e) {
                break;
            }
        }
        int h = nl + 4, x = (cols - w) / 2, y = (rows - h) / 2;
        y = y < 1 ? 1 : y;
        screen_fill(x, y, w, h, C_WINDOW);
        screen_fill(x, y, w, 1, C_FOCUS);
        put_text(x + 1, y, x + w, title, C_WINDOW, C_FOCUS);
        for (int i = 0; i < nl; i++) {
            put_text(x + 2, y + 2 + i, x + w - 1, lines[i], C_WTEXT, C_WINDOW);
        }
        int c = x + 2;
        for (int i = 0; i < n; i++) {
            char label[64];
            label_text(buttons[i], label, sizeof label);
            int len = text_cells(label) + 2;
            uint32_t bg = i == focus ? C_FOCUS : 0x304860;
            screen_fill(c, y + h - 1, len, 1, bg);
            screen_text(c, y + h - 1, "[", C_WTEXT, bg);
            put_text(c + 1, y + h - 1, c + len - 1, label, i == focus ? C_WINDOW : C_WTEXT, bg);
            screen_text(c + len - 1, y + h - 1, "]", C_WTEXT, bg);
            ui_hit(c, y + h - 1, len, 1, HIT_DIALOG + i);
            c += len + 1;
        }
        screen_flush();
        int key = key_poll_timeout(-1, -1), mods = keys_modifiers();
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->button == BTN_LEFT && e->pressed) {
                int id = ui_hit_at(e->col, e->row);
                if (id >= HIT_DIALOG && id < HIT_DIALOG + n) {
                    return id - HIT_DIALOG;
                }
                if (e->col < x || e->col >= x + w || e->row < y || e->row >= y + h) {
                    return cancel;
                }
            }
            continue;
        }
        if (key == 27) {
            return cancel;
        }
        if (key == '\n' || key == ' ') {
            return focus;
        }
        if (key == K_LEFT || (key == '\t' && (mods & KEYS_SHIFT))) {
            focus = (focus + n - 1) % n;
        } else if (key == K_RIGHT || key == '\t') {
            focus = (focus + 1) % n;
        } else if (key > 0 && key < 0x80 && isalpha(key)) {
            for (int i = 0; i < n; i++) {
                char label[64];
                if (label_text(buttons[i], label, sizeof label) == tolower(key)) {
                    return i;
                }
            }
        }
    }
}

/* A line typed at the bottom (Rime's input too): 1 and it in f, or 0. */
static int prompt_line(const char *label, struct field *f)
{
    f->cur = f->len;
    for (;;) {
        draw();
        int rows = screen_rows(), cols = screen_cols(), n = text_cells(label);
        screen_fill(0, rows - 1, cols, 1, C_WINDOW);
        screen_text(1, rows - 1, label, C_WTEXT, C_WINDOW);
        draw_field(n + 2, rows - 1, cols - n - 3, f, 1, C_FIELD);
        screen_flush();
        int key = key_poll_timeout(-1, -1), mods = keys_modifiers(), changed;
        if (key == K_POINTER || key == K_SHOWN || key == 0) {
            continue;
        }
        const char *ime_notice;
        if (ime_action(key, &ime_notice)) {
            if (ime_notice) say_toast(ime_notice);
            continue;
        }
        int taken = ime_process_key(key);
        const char *commit = ime_get_commit();
        if (commit && commit[0]) {
            field_insert(f, commit, strlen(commit));
        }
        if (taken) {
            continue;
        }
        if (key == 27) {
            return 0;
        }
        if (key == '\n') {
            return 1;
        }
        field_key(f, real_key(key, mods), mods, &changed);
    }
}

static void error_box(const char *text)
{
    const char *const ok[] = { _("&OK") };
    ask(_("Error"), text, ok, 1, 0, 0);
}

/*
 * Files: opening, saving, closing, and what happens to them on disk.
 */

/* The folder of d's file watched (inotify), as the user. */
static void watch(struct doc *d)
{
    if (ino < 0 || !d->path[0] || d->watch >= 0) {
        return;
    }
    char target[PATH_MAX];
    file_as_user(1);
    file_resolve(d->path, target, sizeof target);
    char *slash = strrchr(target, '/');
    if (slash) {
        *slash = '\0';
        d->watch = inotify_add_watch(ino, target[0] ? target : "/",
                                     IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE);
    }
    file_as_user(0);
}

static void unwatch(struct doc *d)
{
    if (d->watch < 0) {
        return;
    }
    int shared = 0;
    for (int i = 0; i < ndocs; i++) {
        shared |= docs[i] != d && docs[i]->watch == d->watch;
    }
    if (!shared && ino >= 0) {
        inotify_rm_watch(ino, d->watch);
    }
    d->watch = -1;
}

static const char *display_path(const struct doc *d)
{
    return d->path[0] ? d->path : _("Untitled");
}

/* d's file read again into it (TextBuffer::load), with KTextEditor's
 * messages for what was found. 0, or -1 if it could not be read. */
static int load(struct doc *d)
{
    struct docui *u = ui_of(d);
    char *text;
    size_t len, longest;
    struct stat st;
    uint8_t digest[DIGEST_SIZE];
    struct file_format format;
    int flags, err = file_load_encoding(d->path, &text, &len, &format, &flags, &st, digest, u->limit, &longest,
                                        u->reopen_encoding - 1);
    if (err) {
        struct note *n = post(d, NOTE_ERROR, 1, 0,
                              _("The file %s could not be loaded, as it was not possible to read from it.<br />"
                                "Check if you have read access to this file."), display_path(d));
        n->actions[n->nactions++] = ACT_TRY_AGAIN;
        n->actions[n->nactions++] = ACT_CLOSE;
        say("edit: %s: %s", d->path, strerror(err));
        return -1;
    }
    if (doc_take_text(d, text, len) < 0) {
        post(d, NOTE_ERROR, 1, 0, _("No memory to load %s."), display_path(d));
        return -1;
    }
    lines_loaded(d);
    for (int i = u->nnotes - 1; i >= 0; i--) {
        if (u->notes[i].above) drop_note(d, i);
    }
    d->readonly = 0;
    d->format = format;
    d->st = st;
    d->flags = flags;
    u->scope_valid = 0;
    u->longest = longest;
    memcpy(d->digest, digest, sizeof digest);
    if (!st.st_ino) {
        post(d, NOTE_INFO, 1, 1.0, "%s", _("New file"));
    }
    if (flags & FILE_BROKEN) {
        d->readonly = 1;
        struct note *n = post(d, NOTE_WARNING, 1, 0,
             _("The file %s was opened with %s encoding but contained invalid characters.<br />"
               "It is set to read-only mode, as saving might destroy its content.<br />"
               "Reopen with the correct encoding, or explicitly enable read-write mode to edit these bytes."),
             display_path(d), file_encoding_name(&d->format));
        n->actions[n->nactions++] = ACT_REOPEN_ENCODING;
        n->actions[n->nactions++] = ACT_READ_WRITE;
    }
    if (flags & FILE_WRAPPED) {
        d->readonly = 1;
        struct note *n = post(d, NOTE_WARNING, 1, 0,
                              _("The file %s was opened and contained lines longer than the configured Line Length "
                                "Limit (%zu characters).<br />The longest of those lines was %zu characters long<br/>"
                                "Those lines were wrapped and the document is set to read-only mode, as saving will "
                                "modify its content."), display_path(d), u->limit, longest);
        n->actions[n->nactions++] = ACT_RAISE_LIMIT;
        n->actions[n->nactions++] = ACT_CLOSE;
    }
    d->disk = d->disk_shown = DISK_NONE;
    doc_detect_syntax(d);
    if (d->lang >= 0 && !d->syntax) {
        say("edit: %s: no highlighting: %s", d->path, d->syntax_why);
    }
    return 0;
}

/* documentReload: the text again, undo cleared, each view's cursor kept at
 * its line and column. */
static void reload(struct doc *d)
{
    size_t lines[MAX_SPACES], cols[MAX_SPACES];
    for (int s = 0; s < nspaces; s++) {
        int i = tab_index(s, d);
        if (i >= 0) {
            doc_line_col(d, spaces[s].tabs[i].cursor, &lines[s], &cols[s]);
        }
    }
    /* The marks stay on their lines (documentReload). */
    struct anchors *bm = &ui_of(d)->bookmarks;
    size_t nmarks = bm->n, *marks = nmarks ? malloc(nmarks * sizeof *marks) : NULL;
    for (size_t i = 0; marks && i < nmarks; i++) {
        marks[i] = line_of(d, bm->at[i]);
    }
    unwatch(d);
    load(d);
    watch(d);
    for (size_t i = 0; marks && i < nmarks; i++) {
        if (marks[i] < d->buf.lines) {
            bookmark_set(d, marks[i], 1);
        }
    }
    free(marks);
    for (int s = 0; s < nspaces; s++) {
        int i = tab_index(s, d);
        if (i >= 0) {
            struct tab *t = &spaces[s].tabs[i];
            size_t l = lines[s] < d->buf.lines ? lines[s] : d->buf.lines - 1;
            t->cursor = t->anchor = line_start(d, l) + min_z(cols[s], buffer_line_length(&d->buf, l));
            t->cursor = t->anchor = buffer_prev_cluster(&d->buf, buffer_next_cluster(&d->buf, t->cursor)) == t->cursor ?
                                    t->cursor : buffer_prev_cluster(&d->buf, t->cursor);
            clamp_top(s, t);
        }
    }
}

/* Public action hooks within this translation unit: menus and warning
 * buttons call the same controls, including cancellation and safety checks. */
static void reopen_encoding(struct doc *d)
{
    if (!d || !d->path[0]) return;
    struct field choice = { 0 };
    const char *current = file_encoding_name(&d->format);
    field_set(&choice, current, strlen(current));
    choice.all = 1;
    if (!prompt_line(_("Reopen encoding: Auto, UTF-8, UTF-16LE, UTF-16BE, GB18030, ISO-8859-1, WINDOWS-1252"), &choice)) return;
    int encoding = -2;
    if (strcasecmp(choice.s, "Auto") == 0) encoding = -1;
    for (int i = 0; i < ENC_COUNT; i++) {
        struct file_format f = { .encoding = i };
        if (strcasecmp(choice.s, file_encoding_name(&f)) == 0) encoding = i;
    }
    if (encoding == -2) {
        error_box(_("Unknown encoding. Choose one of the encodings listed in the prompt."));
        return;
    }
    if (doc_modified(d)) {
        const char *const go[] = { _("&Discard Changes and Reload"), _("&Cancel") };
        if (ask(_("Reopen with Encoding"), _("Reloading discards unsaved changes. Continue?"), go, 2, 1, 1) != 0) return;
    }
    ui_of(d)->reopen_encoding = encoding + 1;
    reload(d);
}

static void toggle_readonly(struct doc *d)
{
    if (!d) return;
    if (d->readonly && (d->flags & (FILE_BROKEN | FILE_WRAPPED))) {
        const char *const go[] = { _("Enable Read-&Write Mode"), _("&Cancel") };
        if (ask(_("Possible Data Loss"), _("This document contains invalid encoded bytes or lines wrapped while loading.\n"
                "Saving may change its content. Reopen with the correct encoding or raise the line limit instead.\n"
                "Enable editing anyway?"), go, 2, 1, 1) != 0) return;
    }
    d->readonly = !d->readonly;
    if (!d->readonly) drop_notes_with(d, ACT_READ_WRITE);
}

static struct doc *doc_for_path(const char *path)
{
    for (int i = 0; i < ndocs; i++) {
        if (docs[i]->path[0] && strcmp(docs[i]->path, path) == 0) {
            return docs[i];
        }
    }
    return NULL;
}

/* An absolute path for a file named on the command line. */
static void absolute(const char *path, char *out, size_t size)
{
    char real[PATH_MAX], dir[PATH_MAX];
    if (realpath(path, real)) {
        snprintf(out, size, "%s", real);
        return;
    }
    if (path[0] == '/' || !getcwd(dir, sizeof dir)) {
        snprintf(out, size, "%s", path);
    } else {
        if (snprintf(out, size, "%s/%s", dir, path) >= (int)size) {
            snprintf(out, size, "%s", path); /* too long to join: as given */
        }
    }
}

/* The project of the active document shown in the tree, its file selected. */
static void follow_project(void)
{
    struct tab *t = cur_tab();
    if (!t || !t->doc->path[0]) {
        return;
    }
    struct docui *u = ui_of(t->doc);
    if (!u->project) {
        u->project = project_for(t->doc->path);
    }
    if (u->project) {
        if (project != u->project) tree_top = 0;
        project = u->project;
        refresh_project();
        struct node *n = node_find(project, t->doc->path);
        int count = tree_rows(project, tree_nodes, tree_depth, 8192);
        for (int i = 0; n && i < count; i++) {
            if (tree_nodes[i] == n) {
                tree_sel = i;
            }
        }
        tree_clamp(1);
    }
}

/* KateViewManager::openUrl in the active space: a file already open is
 * shown; a single unmodified Untitled document is replaced. */
static struct doc *open_file(const char *path)
{
    char abs[PATH_MAX];
    absolute(path, abs, sizeof abs);
    struct doc *d = doc_for_path(abs);
    if (d) {
        show_doc(active, d);
        follow_project();
        return d;
    }
    struct doc *replace = NULL;
    if (nspaces == 1 && spaces[0].ntabs == 1) {
        struct doc *o = spaces[0].tabs[0].doc;
        if (!doc_modified(o) && !o->path[0] && doc_in_spaces(o) == 1) {
            replace = o;
        }
    }
    d = new_doc();
    if (!d) {
        return NULL;
    }
    snprintf(d->path, sizeof d->path, "%s", abs);
    update_name(d);
    load(d);
    watch(d);
    show_doc(active, d);
    if (replace) {
        int i = tab_index(active, replace);
        if (i >= 0) {
            drop_tab(active, i);
        }
        free_doc(replace);
        show_doc(active, d);
    }
    follow_project();
    return d;
}

static int save_as(struct doc *d);
static int query_close(struct doc *d);

/* documentSave: Save As for a document without a file (or read-only). */
static int save(struct doc *d)
{
    if (!d->path[0] || d->readonly) {
        return save_as(d);
    }
    if (d->disk != DISK_NONE) {
        const char *const go[] = { _("Save Nevertheless"), _("&Cancel") };
        char text[PATH_MAX + 400];
        const char *why;
        if (d->disk == DISK_DELETED) {
            why = doc_modified(d) ?
                _("The file '%s' was deleted or moved on disk.\n\nDo you really want to save this file? Both your open file and the file on disk were changed. There could be some data lost.") :
                _("The file '%s' was deleted or moved on disk.\n\nDo you really want to save this unmodified file? You could overwrite changed data in the file on disk.");
        } else {
            why = doc_modified(d) ?
                _("The file '%s' was modified on disk.\n\nDo you really want to save this file? Both your open file and the file on disk were changed. There could be some data lost.") :
                _("The file '%s' was modified on disk.\n\nDo you really want to save this unmodified file? You could overwrite changed data in the file on disk.");
        }
        snprintf(text, sizeof text, why, d->path);
        if (ask(doc_modified(d) ? _("Possible Data Loss") : _("Trying to Save Unmodified File"), text, go, 2, 0, 1) != 0) {
            return -1;
        }
    }
    if (language_before_save(d)) return 0;
    save_cleanups(d); /* Remove Spaces, then Newline at End of File, as one edit */
    struct stat st;
    char final[PATH_MAX];
    uint8_t digest[DIGEST_SIZE];
    int err = file_save(d->path, buffer_text(&d->buf), doc_len(d), &d->format, &st, final, sizeof final, digest);
    if (err) {
        char text[PATH_MAX + 400];
        snprintf(text, sizeof text,
                 _("The document could not be saved, as it was not possible to write to %s.\nCheck that you have write "
                   "access to this file or that enough disk space is available.\nThe original file may be lost or "
                   "damaged. Don't quit the application until the file is successfully written."), d->path);
        say("edit: saving %s: %s", d->path, strerror(err));
        error_box(text);
        return -1;
    }
    d->st = st;
    memcpy(d->digest, digest, sizeof digest);
    doc_mark_saved(d);
    lines_saved(d);
    d->disk = d->disk_shown = DISK_NONE;
    drop_notes_with(d, ACT_IGNORE);
    say("edit: saved %s", final);
    language_saved(d);
    return 0;
}

/* documentSaveAs: a folder (files_pick), then the name; a file that is there
 * replaced only when the user says so. */
static int save_as(struct doc *d)
{
    if (d->flags & (FILE_BROKEN | FILE_WRAPPED)) {
        const char *const go[] = { _("&Save Copy"), _("&Cancel") };
        if (ask(_("Possible Data Loss"), _("This document contains invalid encoded bytes or lines wrapped while loading.\n"
                "The saved copy may differ from the original. Reopen with the correct encoding or raise the line "
                "limit for an intact copy. Continue?"), go, 2, 1, 1) != 0) return -1;
    }
    char dir[PATH_MAX], folder[PATH_MAX], home[256] = "", dirs[256];
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    if (d->path[0]) {
        snprintf(dir, sizeof dir, "%s", d->path);
        *strrchr(dir, '/') = '\0';
    } else {
        snprintf(dir, sizeof dir, "%s", home[0] ? home : "/");
    }
    /* TRANSLATORS: (o) is the key to choose a folder in the file picker; keep it. */
    if (!files_pick(dir, NULL, "inode/directory", _("Save As: choose the folder (o), then type the name."), folder,
                    sizeof folder)) {
        return -1;
    }
    struct field name = { 0 };
    if (d->path[0]) {
        field_set(&name, strrchr(d->path, '/') + 1, strlen(strrchr(d->path, '/') + 1));
    } else {
        field_set(&name, ui_of(d)->base, strlen(ui_of(d)->base)); /* the first line, as Kate suggests */
    }
    name.all = 1; /* the name suggested, selected: what is typed replaces it */
    if (!prompt_line(_("File name:"), &name) || !name.s[0] || strchr(name.s, '/')) {
        return -1;
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", folder, name.s) >= (int)sizeof path) {
        return -1;
    }
    struct doc *other = doc_for_path(path);
    if (other && other != d) {
        const char *const go[] = { _("Close Other && Save"), _("&Cancel") };
        char text[PATH_MAX + 300];
        snprintf(text, sizeof text,
                 _("The file %s is already open in another document.\n\nSaving this document there would leave two "
                   "documents for the same file, saving one of them would overwrite the changes of the other one."
                   "\n\nClose the other document and save?"), path);
        if (ask(_("File Already Open"), text, go, 2, 1, 1) != 0 || !query_close(other)) {
            return -1;
        }
        for (int s = 0; s < nspaces; s++) {
            int i = tab_index(s, other);
            if (i >= 0) {
                drop_tab(s, i);
            }
        }
        free_doc(other);
    } else {
        struct stat st;
        file_as_user(1);
        int there = stat(path, &st) == 0;
        file_as_user(0);
        if (there) {
            const char *const go[] = { _("&Overwrite"), _("&Cancel") };
            char text[PATH_MAX + 100];
            snprintf(text, sizeof text, _("The file \"%s\" already exists. Do you wish to overwrite it?"), name.s);
            if (ask(_("Overwrite File?"), text, go, 2, 1, 1) != 0) {
                return -1;
            }
        }
    }
    char old[PATH_MAX];
    snprintf(old, sizeof old, "%s", d->path);
    unwatch(d);
    int readonly = d->readonly, disk = d->disk, disk_shown = d->disk_shown;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->readonly = 0; /* Save As writes a chosen copy, even in read-only mode. */
    d->disk = DISK_NONE;
    if (save(d) < 0) {
        snprintf(d->path, sizeof d->path, "%s", old);
        d->readonly = readonly;
        d->disk = disk;
        d->disk_shown = disk_shown;
        watch(d);
        return -1;
    }
    d->readonly = readonly;
    update_name(d);
    watch(d);
    doc_detect_syntax(d);
    ui_of(d)->project = NULL;
    follow_project();
    return 0;
}

static void save_all(void)
{
    for (int i = 0; i < ndocs; i++) {
        if (doc_modified(docs[i])) {
            save(docs[i]);
        }
    }
}

/* queryClose: 1 when d may be closed (saved, discarded, or not modified). */
static int query_close(struct doc *d)
{
    if (d->disk != DISK_NONE && d->path[0]) {
        const char *const go[] = { _("Close Nevertheless"), _("&Cancel") };
        char text[PATH_MAX + 300];
        snprintf(text, sizeof text,
                 d->disk == DISK_DELETED ?
                     _("The file '%s' was deleted or moved on disk.\n\nDo you really want to continue to close this file? Data loss may occur.") :
                     _("The file '%s' was modified on disk.\n\nDo you really want to continue to close this file? Data loss may occur."),
                 d->path);
        if (ask(_("Possible Data Loss"), text, go, 2, 1, 1) != 0) {
            return 0;
        }
    }
    if (!doc_modified(d) || (doc_len(d) == 0 && !d->path[0])) {
        return 1;
    }
    const char *const go[] = { _("&Save"), _("&Discard"), _("&Cancel") };
    char name[256], text[600];
    doc_name(d, name, sizeof name);
    snprintf(text, sizeof text, _("The document \"%s\" has been modified.\nDo you want to save your changes or discard them?"),
             name);
    int a = ask(_("Close Document"), text, go, 3, 0, 2);
    if (a == 0) {
        return save(d) == 0;
    }
    return a == 1;
}

/* An emptied space: taken away, or (the last one) the welcome view. */
static void space_emptied(int s)
{
    if (spaces[s].ntabs == 0 && nspaces > 1) {
        drop_space(s);
    }
}

/* Ctrl+W (KateViewSpace::closeDocument): the document, if this is its only
 * space, else just its view here. */
static void close_tab(int s, int i)
{
    struct doc *d = spaces[s].tabs[i].doc;
    if (doc_in_spaces(d) > 1) {
        drop_tab(s, i);
        space_emptied(s);
        return;
    }
    if (!query_close(d)) {
        return;
    }
    i = tab_index(s, d);
    if (i >= 0) {
        drop_tab(s, i);
    }
    free_doc(d);
    space_emptied(s);
    follow_project();
}

/* A document closed everywhere (the prompt's Close File): 1, or 0 if the
 * user kept it. */
static int close_doc(struct doc *d)
{
    if (!query_close(d)) {
        return 0;
    }
    for (int s = nspaces - 1; s >= 0; s--) {
        int i = tab_index(s, d);
        if (i >= 0) {
            drop_tab(s, i);
        }
    }
    free_doc(d);
    for (int s = nspaces - 1; s >= 0; s--) {
        space_emptied(s);
    }
    return 1;
}

/* Ctrl+Q (queryClose_internal): documents changed both here and on disk
 * first (Documents Modified on Disk), then the modified ones (Kate's Save
 * Documents dialog, a question for each here). 1: quit. */
static int query_quit(void)
{
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i];
        if (d->disk == DISK_NONE || !doc_modified(d)) {
            continue;
        }
        const char *const go[] = { _("&Ignore Changes"), _("&Overwrite"), _("&Reload") };
        static const char *const states[] = { "", N_("Modified"), N_("Created"), N_("Deleted") };
        char text[PATH_MAX + 300];
        snprintf(text, sizeof text, _("The documents listed below have changed on disk.\n\n%s   %s"), d->path,
                 _(states[d->disk]));
        int a = ask(_("Documents Modified on Disk"), text, go, 3, 2, -1);
        if (a < 0) {
            return 0;
        }
        if (a == 0) {
            d->disk = DISK_NONE;
        } else if (a == 1) {
            d->disk = DISK_NONE;
            if (save(d) < 0) {
                return 0;
            }
        } else {
            reload(d);
        }
    }
    int modified = 0;
    for (int i = 0; i < ndocs; i++) {
        modified += doc_modified(docs[i]) && !(doc_len(docs[i]) == 0 && !docs[i]->path[0]);
    }
    if (modified == 0) {
        return 1;
    }
    char text[4096];
    int n = snprintf(text, sizeof text, "%s", modified == 1 ? "" :
                     _("The following documents have been modified. Do you want to save them before closing?\n"));
    for (int i = 0; i < ndocs && n < (int)sizeof text - 300; i++) {
        struct doc *d = docs[i];
        if (doc_modified(d) && !(doc_len(d) == 0 && !d->path[0])) {
            char name[256];
            doc_name(d, name, sizeof name);
            if (modified == 1) {
                n += snprintf(text + n, sizeof text - (size_t)n,
                              _("The document \"%s\" has been modified. Do you want to save your changes or discard them?"),
                              name);
            } else {
                n += snprintf(text + n, sizeof text - (size_t)n, "\n%s   %s", name, d->path[0] ? d->path : "");
            }
        }
    }
    const char *const go[] = { _("&Save"), _("&Discard"), _("&Cancel") };
    int a = ask(modified == 1 ? _("Close Document") : _("Save Documents"), text, go, 3, 2, 2);
    if (a == 2) {
        return 0;
    }
    for (int i = 0; a == 0 && i < ndocs; i++) {
        if (doc_modified(docs[i]) && !(doc_len(docs[i]) == 0 && !docs[i]->path[0]) && save(docs[i]) < 0) {
            return 0;
        }
    }
    return 1;
}

/* Kate's checksum check and its "Auto Reload If State Is In Version
 * Control": an unmodified document whose old text git has is reloaded. */
static int old_text_in_git(struct doc *d)
{
    char hex[2 * DIGEST_SIZE + 1], dir[PATH_MAX];
    file_digest_hex(d->digest, hex);
    snprintf(dir, sizeof dir, "%s", d->path);
    *strrchr(dir, '/') = '\0';
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        close_range(3, ~0U, 0);
        char *args[] = { "git", "cat-file", "-e", hex, NULL };
        char *env[] = { "PATH=/usr/local/bin:/usr/bin", "LC_ALL=C", "HOME=/", NULL };
        if (setgroups(0, NULL) < 0 || setgid(EDIT_USER) < 0 || setuid(EDIT_USER) < 0 || chdir(dir) < 0) {
            _exit(127);
        }
        execve("/usr/bin/git", args, env);
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* The file of d looked at after inotify saw its name (slotDelayedHandleModOnHd
 * and slotModifiedOnDisk). */
static void check_disk(struct doc *d)
{
    struct docui *u = ui_of(d);
    char target[PATH_MAX];
    struct stat st;
    file_as_user(1);
    file_resolve(d->path, target, sizeof target);
    int there = stat(target, &st) == 0;
    file_as_user(0);
    int reason;
    if (!there) {
        reason = d->st.st_ino || d->disk == DISK_MODIFIED ? DISK_DELETED : DISK_NONE;
    } else {
        uint8_t digest[DIGEST_SIZE];
        int err = file_digest(target, digest);
        if (err == EAGAIN) {
            return; /* still being written: looked at again at the next event */
        }
        u->check = 0;
        if (!err && memcmp(digest, d->digest, DIGEST_SIZE) == 0 && d->st.st_ino) {
            reason = DISK_NONE; /* the same bytes: no change (a touch, or edit's own save) */
        } else {
            reason = d->disk == DISK_DELETED || !d->st.st_ino ? DISK_CREATED : DISK_MODIFIED;
        }
    }
    u->check = 0;
    if (reason == DISK_NONE) {
        if (d->disk == DISK_DELETED && there) {
            d->disk = d->disk_shown = DISK_NONE;
            drop_notes_with(d, ACT_IGNORE);
        }
        return;
    }
    if (reason != DISK_DELETED && !doc_modified(d) && (d->auto_reload || (d->st.st_ino && old_text_in_git(d)))) {
        say("edit: %s changed on disk: reloaded", d->path);
        reload(d);
        return;
    }
    d->disk = reason;
    if (reason == d->disk_shown) {
        return; /* not asked again for the same */
    }
    d->disk_shown = reason;
    drop_notes_with(d, ACT_IGNORE);
    struct note *n = post(d, NOTE_INFO, 1, 0, reason == DISK_DELETED ? _("The file '%s' was deleted or moved on disk.") :
                                                                     _("The file '%s' was modified on disk."), d->path);
    if (reason == DISK_DELETED) {
        n->actions[n->nactions++] = ACT_CLOSE_FILE;
        n->actions[n->nactions++] = ACT_SAVE_AS;
    } else {
        n->actions[n->nactions++] = ACT_AUTO_RELOAD;
        n->actions[n->nactions++] = ACT_RELOAD;
    }
    n->actions[n->nactions++] = ACT_IGNORE;
}

/* inotify's events: the documents whose names they are, looked at. */
static void disk_events(void)
{
    if (projects_changed()) refresh_project();
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(ino, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            for (int i = 0; e->len && i < ndocs; i++) {
                struct doc *d = docs[i];
                char target[PATH_MAX];
                if (d->watch != e->wd || !d->path[0]) {
                    continue;
                }
                file_resolve(d->path, target, sizeof target);
                if (strcmp(strrchr(target, '/') + 1, e->name) == 0) {
                    ui_of(d)->check = 1;
                }
            }
            p += sizeof *e + e->len;
        }
    }
    for (int i = 0; i < ndocs; i++) {
        if (ui_of(docs[i])->check) {
            check_disk(docs[i]);
        }
    }
}

/* A note's action. */
static void note_action(struct doc *d, int note, int action)
{
    struct docui *u = ui_of(d);
    switch (action) {
    case ACT_RELOAD:
    case ACT_TRY_AGAIN:
        d->disk = d->disk_shown = DISK_NONE;
        reload(d);
        break;
    case ACT_AUTO_RELOAD:
        d->auto_reload = 1;
        d->disk = d->disk_shown = DISK_NONE;
        reload(d);
        break;
    case ACT_IGNORE:
    case ACT_CLOSE:
        if (note < u->nnotes) {
            drop_note(d, note);
        }
        break;
    case ACT_CLOSE_FILE: {
        /* Without the prompt about the file on disk (onModOnHdClose), with
         * the one about unsaved changes. */
        int disk = d->disk;
        d->disk = DISK_NONE;
        if (!close_doc(d)) {
            d->disk = disk;
        }
        break;
    }
    case ACT_SAVE_AS:
        if (save_as(d) == 0) {
            drop_notes_with(d, ACT_SAVE_AS);
        }
        break;
    case ACT_REOPEN_ENCODING:
        reopen_encoding(d);
        break;
    case ACT_READ_WRITE:
        toggle_readonly(d);
        break;
    case ACT_RAISE_LIMIT: {
        /* The next power of two above the longest line. */
        size_t limit = 1;
        while (limit <= u->longest) {
            limit *= 2;
        }
        u->limit = limit;
        reload(d);
        break;
    }
    }
}

/*
 * Spaces, tabs and the sidebar.
 */

static void new_document(void)
{
    struct doc *d = new_doc();
    if (d) {
        show_doc(active, d);
    }
}

/* Ctrl+O (slotDocumentOpen): a file chosen; past 10 MiB, Kate asks first. */
static void open_dialog(void)
{
    char dir[PATH_MAX], path[PATH_MAX], home[256] = "", dirs[256];
    struct tab *t = cur_tab();
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    if (t && t->doc->path[0]) {
        snprintf(dir, sizeof dir, "%s", t->doc->path);
        *strrchr(dir, '/') = '\0';
    } else {
        snprintf(dir, sizeof dir, "%s", home[0] ? home : "/");
    }
    /* TRANSLATORS: (Enter) is the key to choose a file in the file picker; keep it. */
    if (!files_pick(dir, NULL, "", _("Open File: choose a file (Enter)."), path, sizeof path)) {
        return;
    }
    struct stat st;
    file_as_user(1);
    int big = stat(path, &st) == 0 && st.st_size > 10L * 1024 * 1024;
    file_as_user(0);
    if (big) {
        const char *const go[] = { _("C&ontinue"), _("Stop") };
        char text[PATH_MAX + 300];
        snprintf(text, sizeof text,
                 _("You are attempting to open one or more large files:\n  %s (%lldMB)\nDo you want to proceed?\n"
                   "Beware that edit may stop responding for some time when opening large files."),
                 strrchr(path, '/') + 1, (long long)(st.st_size / 1024 / 1024));
        if (ask(_("Opening Large File"), text, go, 2, 0, 1) != 0) {
            return;
        }
    }
    open_file(path);
}

/* Ctrl+Shift+L: the active space split in two, side by side, the new one
 * at its right showing the same document, and active. */
static void split_direction(int vertical)
{
    if (nspaces == MAX_SPACES) {
        return;
    }
    if (!view_layout.root) {
        if (edit_layout_init(&view_layout) < 0) return;
        for (int i = 1; i < nspaces; i++) {
            if (edit_layout_split(&view_layout, i - 1, i, 1) < 0) return;
        }
    }
    struct edit_rect r = space_rect(active);
    if ((vertical && r.w < 12) || (!vertical && r.h < 9)) return;
    if (edit_layout_split(&view_layout, active, active + 1, vertical) < 0) return;
    split_drag = NULL;
    struct tab *t = cur_tab();
    memmove(&spaces[active + 2], &spaces[active + 1], (size_t)(nspaces - active - 1) * sizeof spaces[0]);
    nspaces++;
    struct space *old = &spaces[active], *sp = &spaces[active + 1];
    memset(sp, 0, sizeof *sp);
    sp->cur = -1;
    sp->share = old->share / 2 > 0 ? old->share / 2 : 1;
    old->share -= sp->share;
    old->share = old->share > 0 ? old->share : 1;
    active++;
    if (t) {
        show_doc(active, t->doc);
    }
}

static void split(void)
{
    split_direction(1);
}

static void font_zoom(int change, int reset)
{
    struct tab *t = cur_tab();
    if (!t) return;
    if (reset) {
        t->font_px = 0;
    } else {
        int cw, ch, top;
        screen_cells(&cw, &ch, &top);
        int size = (t->font_px ? t->font_px : ch - 2) + change * 2;
        size = size < CONTENT_FONT_MIN_PX ? CONTENT_FONT_MIN_PX :
               size > CONTENT_FONT_MAX_PX ? CONTENT_FONT_MAX_PX : size;
        if (!editor_font) editor_font = content_font_new(NULL, size);
        if (!editor_font || content_font_set_size_px(editor_font, size) < 0) {
            post(t->doc, NOTE_ERROR, 0, 4, "%s", _("The content font could not be loaded."));
            return;
        }
        t->font_px = size;
    }
    t->sub = 0;
    show_cursor(active, t, 0);
}

static void tab_step(int dir)
{
    struct space *sp = &spaces[active];
    if (sp->ntabs > 0) {
        show_tab(active, ((sp->cur < 0 ? 0 : sp->cur) + dir + sp->ntabs) % sp->ntabs); /* Cycle To First Tab */
        follow_project();
    }
}

/* Show Sidebars: the tool view last shown. The Projects tree takes the keys
 * when shown; the Documents list never does (KateFileTree is Qt::NoFocus). */
static void toggle_sidebar(void)
{
    sidebar = !sidebar;
    tree_focus = sidebar && side_view == SIDE_PROJECTS;
    if (sidebar) {
        follow_project();
    }
}

/* View > Tool Views > Documents or Projects: shown, or hidden if shown. */
static void show_tool_view(int view)
{
    if (sidebar && side_view == view) {
        toggle_sidebar();
        return;
    }
    side_view = view;
    sidebar = 0;
    toggle_sidebar();
}

/* The tree's selected row opened: a file shown, a folder opened or closed. */
static void tree_open(int row, int toggle_only)
{
    int n = tree_rows(project, tree_nodes, tree_depth, 8192);
    if (row < 0 || row >= n) {
        return;
    }
    struct node *nd = tree_nodes[row];
    if (nd->dir) {
        if (nd->expanded) {
            nd->expanded = 0;
        } else {
            node_expand(project, nd);
        }
        return;
    }
    if (!toggle_only) {
        char path[PATH_MAX];
        node_path(project, nd, path, sizeof path);
        open_file(path);
    }
}

static void tree_key(int key)
{
    tree_clamp(0);
    int n = tree_rows(project, tree_nodes, tree_depth, 8192), h = screen_rows() - 3;
    if (key == 27) {
        tree_focus = 0;
        return;
    }
    if (!n) return;
    if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN || key == K_BOL || key == K_EOL) {
        int step = key == K_PGUP || key == K_PGDN ? h : 1;
        tree_sel = key == K_BOL ? 0 : key == K_EOL ? n - 1 : tree_sel + (key == K_UP || key == K_PGUP ? -step : step);
        tree_sel = tree_sel >= n ? n - 1 : tree_sel < 0 ? 0 : tree_sel;
    } else if (key == K_RIGHT && tree_sel < n && tree_nodes[tree_sel]->dir && !tree_nodes[tree_sel]->expanded) {
        node_expand(project, tree_nodes[tree_sel]);
    } else if (key == K_LEFT && tree_sel < n) {
        struct node *nd = tree_nodes[tree_sel];
        if (nd->dir && nd->expanded) {
            nd->expanded = 0;
        } else {
            for (int i = tree_sel - 1; i >= 0; i--) {
                if (tree_nodes[i] == nd->parent) {
                    tree_sel = i;
                    break;
                }
            }
        }
    } else if (key == '\n' || key == ' ') {
        tree_open(tree_sel, 0);
    }
    tree_clamp(1);
}

/*
 * The pointer.
 */

/* The text position under the pointer in space s: 1 and *at, or 0. With
 * clamp, a point above or below the text is its first or last row. */
static int point_at(int s, const struct pointer_event *e, size_t *at, int clamp)
{
    struct tab *t = tab_of(s);
    int x0, w, y0, h;
    if (!t) {
        return 0;
    }
    space_cols(s, &x0, &w);
    text_rows(s, &y0, &h);
    w -= scroll_cols(s, t);
    int row = e->row - y0;
    int g = gutter(t), cw, ch, top, col, bw, bh, base_top;
    screen_cells(&bw, &bh, &base_top);
    int own_font = content_metrics(s, &cw, &ch, &top);
    if (own_font) {
        int dy = e->y - base_top - y0 * bh;
        row = dy < 0 ? -1 : dy / ch;
        col = (e->x - x0 * bw - g * cw + cw / 2) / cw;
    } else if (cell_px(&cw, &ch, &top)) {
        col = (e->x - (x0 + g) * cw + cw / 2) / cw;
    } else {
        col = e->col - x0 - g;
    }
    if (!clamp && (row < 0 || row >= h || e->col < x0 || e->col >= x0 + w)) {
        return 0;
    }
    row = row < 0 ? 0 : row >= h ? h - 1 : row;
    col = col < 0 ? 0 : col;
    size_t line = line_of(t->doc, t->top);
    int r = t->sub;
    move_rows(t, text_width(s, t), &line, &r, row);
    *at = at_column(t, text_width(s, t), line, r, col + t->left, 1);
    return 1;
}

static int space_at(int col, int row)
{
    for (int s = 0; s < nspaces; s++) {
        struct edit_rect r = space_rect(s);
        if (col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h) {
            return s;
        }
    }
    return -1;
}

/* The selection a drag makes: by characters, whole words or whole lines,
 * from what the press selected. */
static void drag_to(struct tab *t, size_t at)
{
    if (mc_drag(t, at)) return;
    struct doc *d = t->doc;
    if (drag.mode == DRAG_CHARS) {
        t->cursor = at;
    } else {
        size_t a = at, b = at;
        if (drag.mode == DRAG_WORDS) {
            word_at(d, at, &a, &b);
        } else {
            size_t l = line_of(d, at);
            a = line_start(d, l);
            b = l + 1 < d->buf.lines ? line_start(d, l + 1) : line_end(d, l);
        }
        if (at < drag.start) {
            t->anchor = drag.end;
            t->cursor = a;
        } else {
            t->anchor = drag.start;
            t->cursor = max_z(b, drag.end);
        }
    }
    t->goal = -1;
}

static void click_text(int s, const struct pointer_event *e)
{
    size_t at;
    if (!point_at(s, e, &at, 0)) {
        return;
    }
    struct tab *t = tab_of(s);
    int x0, width, cw, ch, top, bw, bh, base_top;
    space_cols(s, &x0, &width);
    screen_cells(&bw, &bh, &base_top);
    int own_font = content_metrics(s, &cw, &ch, &top);
    int gutter_end = own_font ? x0 * bw + gutter(t) * cw : (x0 + gutter(t)) * bw;
    if (e->x < gutter_end && fold_marker(t, line_of(t->doc, at))) {
        active = s;
        fold_toggle(t, line_of(t->doc, at), (keys_modifiers() & KEYS_SHIFT) != 0);
        return;
    }
    if (mc_click(s, t, e, at)) return;
    struct doc *d = t->doc;
    active = s;
    bar.focus = FOCUS_VIEW;
    tree_focus = 0;
    doc_safe_point(d);
    if (e->clicks == 2) {
        /* selectWord, then cursorToMatchingBracket(true) on a bracket */
        size_t a, b, other;
        int c = byte_at(d, at);
        if (c > 0 && strchr("()[]{}", c) && match_bracket(d, at, &other)) {
            a = min_z(at, other);
            b = max_z(at, other) + 1;
        } else {
            word_at(d, at, &a, &b);
        }
        t->anchor = a;
        t->cursor = b;
        drag = (typeof(drag)){ DRAG_WORDS, s, a, b, e->row };
        set_primary(t);
    } else if (e->clicks >= 3) {
        /* selectLine: with its line break */
        size_t l = line_of(d, at);
        t->anchor = line_start(d, l);
        t->cursor = l + 1 < d->buf.lines ? line_start(d, l + 1) : line_end(d, l);
        drag = (typeof(drag)){ DRAG_LINES, s, t->anchor, t->cursor, e->row };
        set_primary(t);
    } else if (keys_modifiers() & KEYS_SHIFT) {
        t->cursor = at;
        drag = (typeof(drag)){ DRAG_CHARS, s, t->anchor, t->anchor, e->row };
        set_primary(t);
    } else {
        t->cursor = t->anchor = at;
        drag = (typeof(drag)){ DRAG_CHARS, s, at, at, e->row };
    }
    t->goal = -1;
    bar.inc_start = t->cursor;
}

/* The icon border (KateIconBorder::mousePressEvent, mouseReleaseEvent): a
 * press there does not move the cursor; its release on the line pressed
 * toggles the line's bookmark (the only editable mark: the left button), or
 * shows the mark menu (the right one, Allow Mark Menu being on). */
static struct {
    int on, space;
    size_t line;
} icon_press;

static int icon_border_at(int s, const struct pointer_event *e, size_t *line)
{
    struct tab *t = s >= 0 ? tab_of(s) : NULL;
    if (!t || !t->icons) return 0;
    int x0, w, cw, ch, top, bw, bh, base_top;
    space_cols(s, &x0, &w);
    screen_cells(&bw, &bh, &base_top);
    int own_font = content_metrics(s, &cw, &ch, &top);
    int end = own_font ? x0 * bw + ICON_COLS * cw : (x0 + ICON_COLS) * bw;
    size_t at;
    if (e->x < x0 * bw || e->x >= end || !point_at(s, e, &at, 0)) return 0;
    *line = line_of(t->doc, at);
    return 1;
}

static void icon_border_click(int s, size_t line, int button, const struct pointer_event *e)
{
    struct tab *t = tab_of(s);
    if (!t) return;
    int on = bookmarked(t->doc, line);
    if (button == BTN_RIGHT) {
        char item[64];
        snprintf(item, sizeof item, "%c %s", on ? '*' : ' ', _("Bookmark"));
        const char *items[] = { item, NULL }; /* showMarkMenu: the mark types, checkable */
        if (ui_menu(e->col, e->row, items) != 0) return;
    }
    bookmark_set(t->doc, line, !on);
}

static void pointer(const struct pointer_event *e)
{
    int id = ui_hit_at(e->col, e->row), s = space_at(e->col, e->row);
    if (scroll_dragging(e)) {
        return;
    }
    if (switcher_pointer(id, e)) {
        return;
    }
    if (split_drag || sidebar_drag) {
        if (e->button == BTN_LEFT && !e->pressed) {
            split_drag = NULL;
            sidebar_drag = 0;
        } else if (!e->button && !e->wheel) {
            if (split_drag) edit_layout_resize(split_drag, split_drag_area, e->col, e->row,
                                               split_drag->vertical ? 8 : 4);
            else {
                int max = screen_cols() / 2;
                sidebar_width = e->col < 12 ? 12 : e->col > max ? max : e->col;
            }
        }
        return;
    }
    if (e->button == BTN_LEFT && e->pressed) {
        if (sidebar && e->col == SIDE_W && e->row >= 2) {
            sidebar_drag = 1;
            drag.mode = DRAG_NONE;
            return;
        }
        split_drag = edit_layout_separator(&view_layout, editor_area(), e->col, e->row, &split_drag_area);
        if (split_drag) { drag.mode = DRAG_NONE; return; }
    }
    if (!e->button && !e->wheel) {
        if (drag.mode && (e->buttons & 1)) {
            struct tab *t = tab_of(drag.space);
            size_t at;
            drag.y = e->row;
            if (t && point_at(drag.space, e, &at, 1)) {
                drag_to(t, at);
            }
        } else {
            size_t at;
            scroll_hover(id, e); /* the scrollbar's text preview */
            int over_text = s >= 0 && point_at(s, e, &at, 0);
            screen_cursor(over_text ? CURSOR_TEXT : CURSOR_ARROW);
            if (over_text && tab_of(s)) language_hover_at(tab_of(s)->doc, at);
        }
        return;
    }
    if (e->wheel) {
        if ((keys_modifiers() & KEYS_CTRL) && s >= 0 && tab_of(s)) {
            active = s;
            font_zoom(e->wheel > 0 ? 1 : -1, 0);
            return;
        }
        if (sidebar && e->col < SIDE_W) {
            if (side_view == SIDE_DOCUMENTS) {
                docs_wheel(-e->wheel * WHEEL_LINES);
                return;
            }
            tree_top -= e->wheel * WHEEL_LINES;
            tree_clamp(0);
            return;
        }
        struct tab *t = s >= 0 ? tab_of(s) : NULL;
        if (t) {
            int y, h;
            text_rows(s, &y, &h);
            scroll_rows(s, t, (keys_modifiers() & KEYS_SHIFT ? h : WHEEL_LINES) * -e->wheel);
        }
        return;
    }
    size_t icon_line;
    if (icon_press.on && !e->pressed && (e->button == BTN_LEFT || e->button == BTN_RIGHT)) {
        icon_press.on = 0;
        if (s == icon_press.space && icon_border_at(s, e, &icon_line) && icon_line == icon_press.line) {
            icon_border_click(s, icon_line, e->button, e);
        }
        return;
    }
    if (e->pressed && (e->button == BTN_LEFT || e->button == BTN_RIGHT) && icon_border_at(s, e, &icon_line)) {
        icon_press.on = 1;
        icon_press.space = s;
        icon_press.line = icon_line;
        active = s;
        preview_hide();
        return;
    }
    if (e->button == BTN_LEFT && !e->pressed) {
        struct tab *t = drag.mode ? tab_of(drag.space) : NULL;
        if (t && drag.mode == DRAG_CHARS && has_sel(t)) {
            set_primary(t); /* the selection the user made, as Kate's release */
        }
        drag.mode = DRAG_NONE;
        return;
    }
    if (e->pressed && (scroll_pointer(id, e) || docs_pointer(id, e))) {
        return; /* the scrollbar, the mini-map, the Documents list */
    }
    if (e->button == BTN_MIDDLE && e->pressed) {
        if (id >= HIT_TAB && id < HIT_TAB + MAX_SPACES * 1000) {
            close_tab((id - HIT_TAB) / 1000, (id - HIT_TAB) % 1000); /* a middle click closes a tab */
        }
        return;
    }
    if (e->button == BTN_MIDDLE && !e->pressed) {
        size_t at;
        if (s >= 0 && point_at(s, e, &at, 0)) {
            /* Paste the selection where clicked (MousePasteAtCursorPosition is off). */
            struct tab *t = tab_of(s);
            active = s;
            t->cursor = t->anchor = at;
            paste(s, t, 1);
        }
        return;
    }
    if (e->button != BTN_LEFT || !e->pressed) {
        return;
    }
    if (id == HIT_TREE_CLOSE) {
        if (sidebar) toggle_sidebar();
        return;
    }
    if (id == HIT_SIDE_TAB + SIDE_DOCUMENTS || id == HIT_SIDE_TAB + SIDE_PROJECTS) {
        side_view = id - HIT_SIDE_TAB;
        tree_focus = side_view == SIDE_PROJECTS;
        if (tree_focus) follow_project();
        return;
    }
    if (id >= HIT_MENU && id < HIT_MENU + 8) {
        editor_menu(id - HIT_MENU);
        return;
    }
    if (id >= HIT_DIALOG) {
        return;
    }
    if (id >= HIT_TABBAR) {
        if (e->clicks == 2) {
            active = id - HIT_TABBAR;
            new_document(); /* a double click on the empty tab bar */
        }
    } else if (id >= HIT_STATUS) {
        active = (id - HIT_STATUS) / 10;
        struct tab *t = cur_tab();
        if ((id - HIT_STATUS) % 10 == 0) {
            open_goto();
        } else if (t) {
            t->doc->overwrite = !t->doc->overwrite;
        }
    } else if (id >= HIT_WELCOME) {
        active = (id - HIT_WELCOME) / 10;
        if ((id - HIT_WELCOME) % 10 == 0) {
            new_document();
        } else {
            open_dialog();
        }
    } else if (id >= HIT_BAR) {
        int item = id - HIT_BAR;
        bar.focus = FOCUS_BAR;
        tree_focus = 0;
        if (item == ITEM_FIND || item == ITEM_REPLACE || item == ITEM_LINE) {
            bar.item = item;
        } else {
            bar.item = item;
            bar_activate(item, 0);
        }
    } else if (id >= HIT_ACTION) {
        int sp = (id - HIT_ACTION) / 100, note = (id - HIT_ACTION) % 100 / 10, k = (id - HIT_ACTION) % 10;
        struct tab *t = tab_of(sp);
        if (t && note < ui_of(t->doc)->nnotes && k < ui_of(t->doc)->notes[note].nactions) {
            note_action(t->doc, note, ui_of(t->doc)->notes[note].actions[k]);
        }
    } else if (id >= HIT_TREE) {
        tree_focus = 1;
        int row = id - HIT_TREE;
        tree_sel = row;
        int n = tree_rows(project, tree_nodes, tree_depth, 8192);
        if (row < n) {
            /* A file opens on a click, as Kate's tree; a folder on a double
             * click or on its mark. */
            int mark_col = 1 + 2 * tree_depth[row];
            if (!tree_nodes[row]->dir || e->clicks == 2 || e->col == mark_col) {
                tree_open(row, 0);
            }
        }
    } else if (id >= HIT_CLOSE) {
        close_tab((id - HIT_CLOSE) / 1000, (id - HIT_CLOSE) % 1000);
    } else if (id >= HIT_TAB) {
        active = (id - HIT_TAB) / 1000;
        show_tab(active, (id - HIT_TAB) % 1000);
        bar.focus = FOCUS_VIEW;
        tree_focus = 0;
        follow_project();
    } else if (s >= 0 && tab_of(s)) {
        click_text(s, e);
    }
}

/*
 * The keys.
 */

static void global_or_view_key(int key, int mods);

/* Kate's actions on the view (it, or its bar, has the focus). 1 if taken. */
static int view_action(int s, struct tab *t, int key, int mods)
{
    if (mc_action(s, t, key, mods)) return 1;
    int shift = (mods & KEYS_SHIFT) != 0, ctrl = (mods & KEYS_CTRL) != 0, alt = (mods & KEYS_ALT) != 0;
    if (ctrl && !alt) {
        switch (key) {
        case 's':
            if (shift) {
                save_as(t->doc);
            } else {
                save(t->doc);
            }
            return 1;
        case 'w':
            if (!shift) {
                close_tab(s, spaces[s].cur);
            }
            return 1;
        case 'z':
            undo_redo(s, t, shift);
            return 1;
        case 'f':
            if (!shift) {
                open_search(0);
            }
            return 1;
        case 'r':
            if (shift) {
                drop_space(active); /* Close Current View */
            } else {
                open_search(1);
            }
            return 1;
        case 'g':
            open_goto();
            return 1;
        case 'h':
            find_selected(s, shift);
            return 1;
        case 'a':
            if (shift) {
                t->anchor = t->cursor; /* Deselect */
            } else {
                t->anchor = 0;
                t->cursor = doc_len(t->doc);
                set_primary(t);
            }
            return 1;
        case 'd':
            comment(s, t, shift ? UNCOMMENT : COMMENT);
            return 1;
        case '/':
            comment(s, t, TOGGLE_COMMENT);
            return 1;
        case 'i':
            indent_selection(t, shift ? -1 : 1);
            show_cursor(s, t, 0);
            return 1;
        case 'k':
            delete_lines(s, t);
            return 1;
        case 'c':
            copy(t, 0);
            return 1;
        case 'x':
            copy(t, 1);
            show_cursor(s, t, 0);
            return 1;
        case 'v':
            paste(s, t, 0);
            return 1;
        case 0x1E: /* Ctrl+Shift+6 */
            to_bracket(s, t, 1);
            return 1;
        case 'b':
            if (!shift) {
                bookmark_toggle(t); /* Set Bookmark */
            }
            return 1;
        }
        if (key == K_CTRL_0 + 6) {
            to_bracket(s, t, 0);
            return 1;
        }
    }
    if (key == K_F1 + 9 && !mods) {
        t->wrap = !t->wrap; /* Dynamic Word Wrap */
        t->sub = 0;
        t->left = 0;
        show_cursor(s, t, 0);
        return 1;
    }
    if (key == K_F1 + 10 && !mods) {
        t->numbers = !t->numbers; /* Show Line Numbers */
        show_cursor(s, t, 0);
        return 1;
    }
    if (key == K_F1 + 5 && !mods) {
        t->icons = !t->icons; /* Show Icon Border */
        show_cursor(s, t, 0);
        return 1;
    }
    if (key == K_F1 + 2) {
        find_next(shift);
        return 1;
    }
    if (alt && !ctrl && !shift && (key == K_PGUP || key == K_PGDN)) {
        bookmark_step(s, t, key == K_PGDN ? 1 : -1); /* Next, Previous Bookmark */
        return 1;
    }
    return 0;
}

/* Keys of the text itself (KateViewInternal, the view's edit actions). */
static void view_key(int s, struct tab *t, int key, int mods)
{
    if (mc_view_key(s, t, key, mods)) return;
    int shift = (mods & KEYS_SHIFT) != 0, ctrl = (mods & KEYS_CTRL) != 0, alt = (mods & KEYS_ALT) != 0;
    struct doc *d = t->doc;
    switch (key) {
    case K_LEFT:
    case K_RIGHT:
        if (ctrl || (alt && shift)) {
            move_to(s, t, key == K_RIGHT ? word_next(d, t->cursor) : word_prev(d, t->cursor), shift, 0);
        } else {
            key_left_right(s, t, key == K_RIGHT, shift);
        }
        return;
    case K_UP:
    case K_DOWN:
        if (ctrl && !shift) {
            scroll_rows(s, t, key == K_UP ? -1 : 1);
        } else if (!ctrl) {
            move_vertical(s, t, key == K_UP ? -1 : 1, shift);
        }
        return;
    case K_PGUP:
    case K_PGDN:
        if (!ctrl && !alt) {
            page(s, t, key == K_PGUP ? -1 : 1, shift);
            show_cursor(s, t, 0);
        }
        return;
    case K_BOL:
    case K_EOL:
        if (ctrl) {
            move_to(s, t, key == K_BOL ? 0 : doc_len(d), shift, 0);
        } else if (alt) {
            view_edge(s, t, key == K_EOL, shift);
        } else if (key == K_BOL) {
            key_home(s, t, shift);
        } else {
            key_end(s, t, shift);
        }
        return;
    case K_DEL:
        if (shift && !ctrl) {
            copy(t, 1);
            show_cursor(s, t, 0);
        } else if (ctrl) {
            delete_word(s, t, 1);
        } else {
            key_delete(s, t);
        }
        return;
    case K_INS:
        if (shift && ctrl) {
            paste(s, t, 1);
        } else if (shift) {
            paste(s, t, 0);
        } else if (ctrl) {
            copy(t, 0);
        } else {
            d->overwrite = !d->overwrite; /* Overwrite Mode */
        }
        return;
    case '\b':
        if (ctrl) {
            delete_word(s, t, 0);
        } else {
            key_backspace(s, t);
        }
        return;
    case '\n':
        newline(s, t, ctrl && alt ? NEWLINE_ABOVE : ctrl && shift ? NEWLINE_BELOW : ctrl ? NEWLINE_PLAIN :
                      shift ? NEWLINE_SMART : NEWLINE_INDENT);
        return;
    case '\t':
        if (ctrl || alt) {
            return;
        }
        if (shift) {
            indent_selection(t, -1);
            show_cursor(s, t, 0);
        } else {
            key_tab(s, t);
        }
        return;
    case 27:
        if (!ctrl && !alt && has_sel(t)) {
            t->anchor = t->cursor; /* Esc: deselect */
        }
        return;
    }
    const char *typed = keys_typed(key);
    if (typed && !ctrl && !alt) {
        type_text(s, t, typed, strlen(typed));
    }
}

/* Main window actions, then the focused part's keys. */
static void global_or_view_key(int key, int mods)
{
    int shift = (mods & KEYS_SHIFT) != 0, ctrl = (mods & KEYS_CTRL) != 0, alt = (mods & KEYS_ALT) != 0;
    if (key == K_F1 + 9 && !ctrl && !alt && shift) {
        editor_menu(0);
        return;
    }
    if (alt && !ctrl && !shift && !(bar_kind && bar.focus == FOCUS_BAR)) {
        const char *mnemonics = EDITOR_MNEMONICS;
        const char *m = key > 0 && key < 0x80 ? strchr(mnemonics, key) : NULL;
        if (m) { editor_menu((int)(m - mnemonics)); return; }
    }
    if (ctrl && !alt && key == 'n' && !shift) {
        new_document();
        return;
    }
    if (ctrl && !alt && key == 'o' && !shift) {
        open_dialog();
        return;
    }
    if (ctrl && alt && !shift && key == 'o') {
        quick_open(); /* View > Quick Open */
        return;
    }
    if (alt && !ctrl && !shift && (key == K_UP || key == K_DOWN)) {
        docs_step(key == K_UP ? -1 : 1); /* View > Previous, Next Document (the Documents list) */
        return;
    }
    if (language_key(key, mods)) return;
    if (ctrl && !alt && key == 'q') {
        if (session_quit()) {
            quitting = 1;
        }
        return;
    }
    if (ctrl && shift && !alt && (key == '+' || key == '=' || key == '-')) {
        if (cur_tab()) fold_top_level(cur_tab(), key == '-', 0);
        return;
    }
    if (ctrl && !alt && (key == '+' || key == '=' || key == '-' || key == '0' || key == K_CTRL_0)) {
        /* Ctrl+0 comes as K_CTRL_0 (keys.c gives Ctrl+digits so) */
        font_zoom(key == '-' ? -1 : 1, key == '0' || key == K_CTRL_0);
        return;
    }
    if (ctrl && !alt && shift && key == 't') {
        split_direction(0);
        return;
    }
    if (ctrl && !alt && key == 'l') {
        if (shift) {
            split(); /* Split Vertical */
        } else {
            save_all();
        }
        return;
    }
    if (ctrl && shift && !alt && key == 'f') {
        filesearch_menu();
        return;
    }
    if (ctrl && alt && shift && key == 'f') {
        toggle_sidebar(); /* Show Sidebars */
        return;
    }
    if (key == K_F1 + 7 && !ctrl && !alt) {
        if (nspaces > 1) {
            active = (active + (shift ? nspaces - 1 : 1)) % nspaces; /* Next, Previous Split View */
            bar.focus = FOCUS_VIEW;
            tree_focus = 0;
            follow_project();
        }
        return;
    }
    if ((alt && !ctrl && !shift && (key == K_LEFT || key == K_RIGHT)) ||
        (ctrl && !alt && !shift && (key == K_PGUP || key == K_PGDN))) {
        tab_step(key == K_RIGHT || key == K_PGDN ? 1 : -1);
        return;
    }
    if (alt && !ctrl && !shift && key >= '0' && key <= '9') {
        int i = key == '0' ? 9 : key - '1'; /* Switch to Tab N */
        if (i < spaces[active].ntabs) {
            show_tab(active, i);
            follow_project();
        }
        return;
    }
    if (tree_focus && sidebar) {
        tree_key(key);
        return;
    }
    struct tab *t = cur_tab();
    if (!t) {
        return;
    }
    if (bar_kind && bar.focus == FOCUS_BAR && bar_key(key, mods)) {
        return;
    }
    if (view_action(active, t, key, mods)) {
        return;
    }
    if (key == 27 && bar_kind && !ctrl && !alt) {
        close_bar(); /* Esc: the bar first */
        return;
    }
    if (bar_kind && bar.focus == FOCUS_BAR) {
        return; /* the bar's: not typed into the text */
    }
    view_key(active, t, key, mods);
}

#include "edit-multicursor.inc"
#include "edit-language.inc"
#include "edit-session-ui.inc"
#include "edit-filesearch-ui.inc"
#include "edit-scrollbar.inc"
#include "edit-documents.inc"
#include "edit-quickopen.inc"
#include "edit-actions.inc"

/* What a key does: process 1's and the input method's first, then Rime's
 * composition, then edit's. */
static void key_event(int key)
{
    int mods = keys_modifiers();
    if (key == K_SHOWN || key == 0) {
        return;
    }
    if (key == K_POINTER) {
        pointer(pointer_last());
        auto_bracket_check();
        return;
    }
    if (switcher_key(real_key(key, mods), mods)) {
        return; /* Ctrl+Tab's list of documents */
    }
    if (primary_pending && !(mods & KEYS_SHIFT)) {
        set_primary(cur_tab()); /* a keyboard selection ended (Kate: on the Shift key's release) */
        primary_pending = 0;
    }
    const char *ime_notice;
    if (ime_action(key, &ime_notice)) {
        if (ime_notice) say_toast(ime_notice);
        return;
    }
    if (ime_mode() == IME_MODE_CN && ime_width_key(key, mods)) {
        const char *msg = ime_toggle_width();
        if (msg) {
            say_toast(msg);
        }
        return;
    }
    if (key == K_ACT_HOME || key == K_ACT_LOCK || (key >= K_VOL_UP && key <= K_BRI_DOWN)) {
        return;
    }
    struct tab *t = cur_tab();
    int text_focus = t && !tree_focus && (bar.focus == FOCUS_VIEW || bar.item == ITEM_FIND ||
                                          bar.item == ITEM_REPLACE);
    ime_set_focus(text_focus);
    if (text_focus) {
        int taken = ime_process_key(key);
        const char *commit = ime_get_commit();
        if (commit && commit[0]) {
            if (bar_kind && bar.focus == FOCUS_BAR) {
                struct field *f = bar.item == ITEM_REPLACE ? &bar.replace : &bar.find;
                field_insert(f, commit, strlen(commit));
                if (f == &bar.find && bar_kind == BAR_FIND) {
                    inc_search(active);
                }
            } else {
                type_text(active, t, commit, strlen(commit));
            }
        }
        if (taken) {
            return;
        }
    }
    global_or_view_key(real_key(key, mods), mods);
    auto_bracket_check();
}

/* Notes that hid themselves, and how long until the next one does. */
static int expire(void)
{
    double t = now(), next = -1;
    for (int i = 0; i < ndocs; i++) {
        struct docui *u = ui_of(docs[i]);
        for (int k = u->nnotes - 1; k >= 0; k--) {
            if (u->notes[k].until > 0 && u->notes[k].until <= t) {
                drop_note(docs[i], k);
            } else if (u->notes[k].until > 0 && (next < 0 || u->notes[k].until < next)) {
                next = u->notes[k].until;
            }
        }
    }
    if (toast[0] && toast_until > t && (next < 0 || toast_until < next)) {
        next = toast_until;
    }
    return next < 0 ? -1 : (int)((next - t) * 1000) + 1;
}

/* Highlighting parsed a slice at a time, for the documents shown. 1 when
 * there is more to do. */
static int parse_shown(void)
{
    int more = 0;
    for (int s = 0; s < nspaces; s++) {
        struct tab *t = tab_of(s);
        if (t && t->doc->syntax && syntax_pending(t->doc->syntax)) {
            more |= syntax_parse(t->doc->syntax, &t->doc->buf, 30) == 0;
        }
    }
    return more;
}

/* One turn: drawn, then an event (or a timeout) handled. */
static void step(void)
{
    language_tick();
    session_tick();
    filesearch_tick();
    int more = parse_shown();
    draw();
    int timeout = session_timeout(expire());
    if ((language_active() || filesearch_active()) && (timeout < 0 || timeout > 50)) timeout = 50;
    if (more) {
        timeout = 0;
    }
    if (primary_pending) {
        timeout = timeout < 0 || timeout > 500 ? 500 : timeout;
    }
    if (switcher_open()) {
        timeout = timeout < 0 || timeout > 50 ? 50 : timeout; /* Ctrl's release has no key of its own */
    }
    int preview_due = preview_timeout(); /* the scrollbar's text preview, 250 ms after the pointer came */
    if (preview_due >= 0 && (timeout < 0 || timeout > preview_due)) timeout = preview_due;
    int y0 = 0, h = 0;
    if (drag.mode && tab_of(drag.space)) {
        text_rows(drag.space, &y0, &h);
        if (drag.y < y0 || drag.y >= y0 + h) {
            timeout = timeout < 0 || timeout > 50 ? 50 : timeout;
        }
    }
    struct pollfd p[2] = { { .fd = ino, .events = POLLIN },
                           { .fd = projects_poll_fd(), .events = POLLIN } };
    int key = key_poll_many(p, 2, timeout);
    if ((p[0].revents | p[1].revents) & POLLIN) {
        disk_events();
    }
    if (key) {
        key_event(key);
        return;
    }
    switcher_tick();
    if (primary_pending && now() - primary_since >= 0.5) {
        set_primary(cur_tab());
        primary_pending = 0;
    }
    struct tab *t = drag.mode ? tab_of(drag.space) : NULL;
    if (t && (drag.y < y0 || drag.y >= y0 + h)) {
        /* Dragged past the text: it scrolls a row each 50 ms. */
        scroll_rows(drag.space, t, drag.y < y0 ? -1 : 1);
        struct pointer_event e = *pointer_last();
        e.row = drag.y < y0 ? y0 : y0 + h - 1;
        size_t at;
        if (point_at(drag.space, &e, &at, 1)) {
            drag_to(t, at);
        }
    }
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "C.UTF-8");
    screen_open(BASE_FONT);
    keys_open();
    if (screen_cols() <= 0 || screen_rows() <= 3) {
        say("edit: no screen");
        return 1;
    }
    ime_init(NULL, NULL);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    spaces[0] = (struct space){ .cur = -1, .share = 1000 };
    if (edit_layout_init(&view_layout) < 0) return 1;
    session_startup();
    for (int i = 1; i < argc; i++) {
        open_file(argv[i]);
    }
    say("edit: started with %d document(s)", ndocs);
    while (!quitting) {
        step();
    }
    filesearch_done();
    language_done();
    ime_done();
    edit_layout_free(&view_layout);
    content_font_free(editor_font);
    editor_font = NULL;
    return 0;
}
