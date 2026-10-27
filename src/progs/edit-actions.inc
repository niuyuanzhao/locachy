/* The real actions exposed by the menu, shared with existing key handlers.
 * Included after editing/navigation functions so no second command path is
 * needed for saving, selection, search or document-change prompts.
 *
 * The menu bar is Kate's at kate dc92991f (apps/lib/data/kateui.rc) with
 * KTextEditor's bed9523 merged in (src/data/katepart5ui.rc) and the default
 * plugins' (addons/{search,filetree,tabswitcher,lspclient}/ui.rc): File, Edit,
 * Selection, Go, View, LSP Client, Tools, Sessions, in that order, each item
 * under the menu and submenu those files give it. Items edit has that Kate
 * keeps out of its menus (they only have shortcuts there) are put with their
 * kind, so that each stays reachable. */
/* Their Alt keys are EDITOR_MNEMONICS (edit.c): &File, &Edit, Sele&ction,
 * &Go, &View, &LSP Client, &Tools, Sess&ions. */
enum { MENU_FILE, MENU_EDIT, MENU_SELECTION, MENU_GO, MENU_VIEW, MENU_LSP, MENU_TOOLS, MENU_SESSIONS,
       EDITOR_MENUS,
       /* The submenus. */
       SUB_FIND_VARIANTS = EDITOR_MENUS, SUB_BOOKMARKS, SUB_SPLIT_VIEW, SUB_TOOL_VIEWS, SUB_WORD_WRAP, SUB_BORDERS,
       SUB_CODE_FOLDING };

static const char *editor_menu_title(int i)
{
    switch (i) {
    case MENU_FILE: return _("File");
    case MENU_EDIT: return _("Edit");
    case MENU_SELECTION: return _("Selection");
    case MENU_GO: return C_("menu", "Go");
    case MENU_VIEW: return C_("menu", "View");
    case MENU_LSP: return _("LSP Client");
    case MENU_TOOLS: return _("Tools");
    case MENU_SESSIONS: return _("Sessions");
    }
    return "";
}
/* The menus' titles are drawn by editor_menu_title. */

enum editor_action {
    EA_NONE, EA_NEW, EA_OPEN, EA_SAVE, EA_SAVE_AS, EA_SAVE_ALL, EA_RELOAD,
    EA_CLOSE, EA_CLOSE_ALL, EA_QUIT, EA_UNDO, EA_REDO, EA_CUT, EA_COPY, EA_PASTE,
    EA_SELECT_ALL, EA_DESELECT, EA_SPLIT_V, EA_SPLIT_H, EA_TOGGLE_SPLIT,
    EA_CLOSE_VIEW, EA_CLOSE_OTHER_VIEWS, EA_NEXT_VIEW, EA_PREV_VIEW, EA_PROJECTS,
    EA_WRAP, EA_NUMBERS, EA_FIND, EA_REPLACE, EA_NEXT_MATCH, EA_PREV_MATCH, EA_GOTO,
    EA_COMMENT, EA_UNCOMMENT, EA_TOGGLE_COMMENT, EA_INDENT, EA_UNINDENT,
    EA_MATCH_BRACKET, EA_READONLY, EA_REOPEN_ENCODING, EA_FONT_UP, EA_FONT_DOWN, EA_FONT_RESET, EA_LANGUAGE, EA_SESSION_MANAGE,
    EA_ADD_ABOVE, EA_ADD_BELOW, EA_LINE_CARETS, EA_NEXT_OCCURRENCE, EA_ALL_OCCURRENCES,
    EA_SKIP_OCCURRENCE, EA_BLOCK, EA_REMOVE_EMPTY, EA_CLEAR_CARETS,
    EA_FOLD_CURRENT, EA_UNFOLD_CURRENT, EA_FOLD_TOP, EA_UNFOLD_TOP, EA_FOLD_ALL, EA_UNFOLD_ALL, EA_FIND_FILES, EA_FILE_RESULTS,
    EA_QUICK_OPEN, EA_LAST_USED, EA_LAST_USED_REVERSE, EA_PREV_DOCUMENT, EA_NEXT_DOCUMENT, EA_DOCUMENTS,
    EA_SCROLLBAR_MARKS, EA_MINIMAP, EA_SET_BOOKMARK, EA_CLEAR_BOOKMARKS, EA_PREV_BOOKMARK, EA_NEXT_BOOKMARK,
    EA_ICON_BORDER, EA_PREV_MODIFIED, EA_NEXT_MODIFIED, EA_SIDEBARS, EA_PASTE_SELECTION, EA_OVERWRITE,
    EA_FIND_SELECTED, EA_FIND_SELECTED_BACK, EA_SELECT_BRACKET, EA_NEXT_TAB, EA_PREV_TAB,
    EA_SUBMENU = 1000,       /* + the submenu: it opens */
    EA_BOOKMARK = 2000       /* + the bookmark's index: the Bookmarks menu's list of them */
};
#define EDITOR_ITEMS 48

struct editor_item { const char *label, *shortcut; int action, checked, enabled; };

/* The bookmarks before and after the cursor's line, as insertBookmarks
 * finds them (-1: none), for the Bookmarks menu's Previous and Next. */
static void bookmark_neighbours(struct tab *t, long *prev, long *next)
{
    struct anchors *a = &ui_of(t->doc)->bookmarks;
    size_t line = line_of(t->doc, t->cursor);
    *prev = *next = -1;
    for (size_t i = 0; i < a->n; i++) {
        size_t l = line_of(t->doc, a->at[i]);
        if (l < line) *prev = (long)l;
        if (l > line && *next < 0) *next = (long)l;
    }
}

/* A line's text for a menu, at most n bytes of it (KStringHandler::rsqueeze's
 * part), tabs as spaces. */
static void line_label(struct doc *d, size_t line, size_t n, char *out, size_t size)
{
    size_t at = line_start(d, line), len = min_z(line_end(d, line) - at, n);
    char *text = buffer_dup(&d->buf, at, len);
    snprintf(out, size, "%s", text ? text : "");
    free(text);
    for (char *p = out; *p; p++) {
        if (*p == '\t') *p = ' ';
    }
}

static int editor_items(int menu, struct editor_item *items)
{
    int n = 0;
    struct tab *t = cur_tab();
    int opened = t != NULL;
#define ACTION(label, keys, action, checked, enabled) \
    (items[n++] = (struct editor_item){ label, keys, action, checked, enabled })
#define SUBMENU(label, sub) ACTION(label, ">", EA_SUBMENU + (sub), 0, 1)
    switch (menu) {
    case MENU_FILE: /* kateui.rc's File, katepart5ui.rc's save_merge and revert_merge */
        ACTION(_("New"), "Ctrl+N", EA_NEW, 0, 1);
        ACTION(_("Open..."), "Ctrl+O", EA_OPEN, 0, 1);
        ACTION(_("Save"), "Ctrl+S", EA_SAVE, 0, opened);
        ACTION(_("Save As..."), "Ctrl+Shift+S", EA_SAVE_AS, 0, opened);
        ACTION(_("Save All"), "Ctrl+L", EA_SAVE_ALL, 0, ndocs > 0);
        ACTION(_("Reload"), "F5", EA_RELOAD, 0, opened && t->doc->path[0]);
        ACTION(_("Close"), "Ctrl+W", EA_CLOSE, 0, opened);
        ACTION(_("Close All"), "", EA_CLOSE_ALL, 0, ndocs > 0);
        ACTION(_("Quit"), "Ctrl+Q", EA_QUIT, 0, 1);
        break;
    case MENU_EDIT: /* katepart5ui.rc's Edit, then the search plugin's Find in Files */
        ACTION(_("Undo"), "Ctrl+Z", EA_UNDO, 0, opened && t->doc->done > 0);
        ACTION(_("Redo"), "Ctrl+Shift+Z", EA_REDO, 0, opened && t->doc->done < t->doc->count);
        ACTION(_("Cut"), "Ctrl+X", EA_CUT, 0, opened && has_sel(t));
        ACTION(_("Copy"), "Ctrl+C", EA_COPY, 0, opened && has_sel(t));
        ACTION(_("Paste"), "Ctrl+V", EA_PASTE, 0, opened && !t->doc->readonly);
        ACTION(_("Paste Selection"), "Ctrl+Shift+Ins", EA_PASTE_SELECTION, 0, opened && !t->doc->readonly);
        ACTION(_("Overwrite Mode"), "Ins", EA_OVERWRITE, opened && t->doc->overwrite, opened);
        ACTION(_("Read Only Mode"), "", EA_READONLY, opened && t->doc->readonly, opened);
        ACTION(_("Find..."), "Ctrl+F", EA_FIND, 0, opened);
        SUBMENU(_("Find Variants"), SUB_FIND_VARIANTS);
        ACTION(_("Replace..."), "Ctrl+R", EA_REPLACE, 0, opened);
        ACTION(_("Find in Files..."), "Ctrl+Shift+F", EA_FIND_FILES, 0, 1);
        ACTION(_("Find in Files Results..."), "", EA_FILE_RESULTS, 0, 1);
        break;
    case SUB_FIND_VARIANTS:
        ACTION(_("Find Next"), "F3", EA_NEXT_MATCH, 0, opened);
        ACTION(_("Find Previous"), "Shift+F3", EA_PREV_MATCH, 0, opened);
        ACTION(_("Find Selected"), "Ctrl+H", EA_FIND_SELECTED, 0, opened);
        ACTION(_("Find Selected Backwards"), "Ctrl+Shift+H", EA_FIND_SELECTED_BACK, 0, opened);
        break;
    case MENU_SELECTION: /* katepart5ui.rc's Selection; the multicursor actions it has no place for last */
        ACTION(_("Select All"), "Ctrl+A", EA_SELECT_ALL, 0, opened);
        ACTION(_("Deselect"), "Ctrl+Shift+A", EA_DESELECT, 0, opened && has_sel(t));
        ACTION(_("Block Selection Mode"), "Ctrl+Shift+B", EA_BLOCK, opened && mc_block(t), opened);
        ACTION(_("Toggle Comment"), "Ctrl+/", EA_TOGGLE_COMMENT, 0, opened);
        ACTION(_("Comment"), "Ctrl+D", EA_COMMENT, 0, opened);
        ACTION(_("Uncomment"), "Ctrl+Shift+D", EA_UNCOMMENT, 0, opened);
        ACTION(_("Add Caret above Cursor"), "Ctrl+Alt+Up", EA_ADD_ABOVE, 0, opened);
        ACTION(_("Add Caret below Cursor"), "Ctrl+Alt+Down", EA_ADD_BELOW, 0, opened);
        ACTION(_("Add Cursors to Line Ends"), "Shift+Alt+I", EA_LINE_CARETS, 0, opened);
        ACTION(_("Find and Select Next Occurrence"), "Alt+J", EA_NEXT_OCCURRENCE, 0, opened);
        ACTION(_("Find and Select All Occurrences"), "Ctrl+Alt+Shift+J", EA_ALL_OCCURRENCES, 0, opened);
        ACTION(_("Mark Currently Selected Occurrence as Skipped"), "Alt+K", EA_SKIP_OCCURRENCE, 0, opened);
        ACTION(_("Remove Cursors from Empty Lines"), "", EA_REMOVE_EMPTY, 0, opened);
        ACTION(_("Remove Additional Cursors"), "Esc", EA_CLEAR_CARETS, 0, opened && mc_count(t) > 1);
        break;
    case MENU_GO: /* kateui.rc's Go with the tab switcher's, the file tree's and katepart5ui.rc's groups */
        ACTION(_("Quick Open"), "Ctrl+Alt+O", EA_QUICK_OPEN, 0, 1);
        ACTION(_("Last Used Views"), "Ctrl+Tab", EA_LAST_USED, 0, ndocs > 1);
        ACTION(_("Last Used Views (Reverse)"), "Ctrl+Shift+Tab", EA_LAST_USED_REVERSE, 0, ndocs > 1);
        ACTION(_("Go to Line..."), "Ctrl+G", EA_GOTO, 0, opened);
        ACTION(_("Previous Document"), "Alt+Up", EA_PREV_DOCUMENT, 0, ndocs > 0);
        ACTION(_("Next Document"), "Alt+Down", EA_NEXT_DOCUMENT, 0, ndocs > 0);
        ACTION(_("Next Tab"), "Alt+Right", EA_NEXT_TAB, 0, spaces[active].ntabs > 1);
        ACTION(_("Previous Tab"), "Alt+Left", EA_PREV_TAB, 0, spaces[active].ntabs > 1);
        ACTION(_("Go to Previous Modified Line"), "", EA_PREV_MODIFIED, 0, opened);
        ACTION(_("Go to Next Modified Line"), "", EA_NEXT_MODIFIED, 0, opened);
        ACTION(_("Go to Matching Bracket"), "Ctrl+6", EA_MATCH_BRACKET, 0, opened);
        ACTION(_("Select to Matching Bracket"), "Ctrl+Shift+6", EA_SELECT_BRACKET, 0, opened);
        SUBMENU(_("Bookmarks"), SUB_BOOKMARKS);
        break;
    case SUB_BOOKMARKS: { /* KateBookmarks::bookmarkMenuAboutToShow and insertBookmarks */
        static char prev_label[128], next_label[128], labels[24][128];
        char text[48];
        long prev = -1, next = -1;
        ACTION(_("Toggle Bookmark"), "Ctrl+B", EA_SET_BOOKMARK, opened && bookmarked(t->doc, line_of(t->doc, t->cursor)), opened);
        ACTION(_("Clear All Bookmarks"), "", EA_CLEAR_BOOKMARKS, 0, opened && ui_of(t->doc)->bookmarks.n);
        if (opened) bookmark_neighbours(t, &prev, &next);
        if (prev >= 0) {
            line_label(t->doc, (size_t)prev, 24, text, sizeof text);
            /* TRANSLATORS: %ld is the bookmark's line number, %s is the start of that line's text. */
            snprintf(prev_label, sizeof prev_label, _("Previous: %ld - \"%s\""), prev + 1, text);
            ACTION(prev_label, "Alt+PgUp", EA_PREV_BOOKMARK, 0, 1);
        }
        if (next >= 0) {
            line_label(t->doc, (size_t)next, 24, text, sizeof text);
            /* TRANSLATORS: %ld is the bookmark's line number, %s is the start of that line's text. */
            snprintf(next_label, sizeof next_label, _("Next: %ld - \"%s\""), next + 1, text);
            ACTION(next_label, "Alt+PgDown", EA_NEXT_BOOKMARK, 0, 1);
        }
        /* Each bookmark: its line number and the start of its text. */
        for (size_t i = 0; opened && i < ui_of(t->doc)->bookmarks.n && n < EDITOR_ITEMS && i < 24; i++) {
            size_t line = line_of(t->doc, ui_of(t->doc)->bookmarks.at[i]);
            line_label(t->doc, line, 40, text, sizeof text);
            snprintf(labels[i], sizeof labels[i], "%zu - \"%s\"", line + 1, text);
            ACTION(labels[i], "", EA_BOOKMARK + (int)i, 0, 1);
        }
        break;
    }
    case MENU_VIEW: /* kateui.rc's View, then katepart5ui.rc's view_operations */
        SUBMENU(_("Split View"), SUB_SPLIT_VIEW);
        SUBMENU(_("Tool Views"), SUB_TOOL_VIEWS);
        ACTION(_("Enlarge Font"), "Ctrl++", EA_FONT_UP, 0, opened);
        ACTION(_("Shrink Font"), "Ctrl+-", EA_FONT_DOWN, 0, opened);
        ACTION(_("Reset Font Size"), "Ctrl+0", EA_FONT_RESET, 0, opened);
        SUBMENU(_("Word Wrap"), SUB_WORD_WRAP);
        SUBMENU(_("Borders"), SUB_BORDERS);
        SUBMENU(_("Code Folding"), SUB_CODE_FOLDING);
        break;
    case SUB_SPLIT_VIEW:
        ACTION(_("Previous Split View"), "Shift+F8", EA_PREV_VIEW, 0, nspaces > 1);
        ACTION(_("Next Split View"), "F8", EA_NEXT_VIEW, 0, nspaces > 1);
        ACTION(_("Split Vertical"), "Ctrl+Shift+L", EA_SPLIT_V, 0, nspaces < MAX_SPACES);
        ACTION(_("Split Horizontal"), "Ctrl+Shift+T", EA_SPLIT_H, 0, nspaces < MAX_SPACES);
        ACTION(_("Toggle Orientation"), "", EA_TOGGLE_SPLIT, 0, nspaces > 1);
        ACTION(_("Close Current View"), "Ctrl+Shift+R", EA_CLOSE_VIEW, 0, nspaces > 1);
        ACTION(_("Close Inactive Views"), "", EA_CLOSE_OTHER_VIEWS, 0, nspaces > 1);
        break;
    case SUB_TOOL_VIEWS: /* KateMDI::GUIClient: Show Sidebars, then Show <tool view> */
        ACTION(_("Show Sidebars"), "Ctrl+Alt+Shift+F", EA_SIDEBARS, sidebar, 1);
        ACTION(_("Show Documents"), "", EA_DOCUMENTS, sidebar && side_view == SIDE_DOCUMENTS, 1);
        ACTION(_("Show Projects"), "", EA_PROJECTS, sidebar && side_view == SIDE_PROJECTS, 1);
        break;
    case SUB_WORD_WRAP:
        ACTION(_("Dynamic Word Wrap"), "F10", EA_WRAP, opened && t->wrap, opened);
        break;
    case SUB_BORDERS:
        ACTION(_("Show Icon Border"), "F6", EA_ICON_BORDER, opened && t->icons, opened);
        ACTION(_("Show Line Numbers"), "F11", EA_NUMBERS, opened && t->numbers, opened);
        ACTION(_("Show Scrollbar Marks"), "", EA_SCROLLBAR_MARKS, opened && t->marks, opened && t->scrollbar);
        ACTION(_("Show Scrollbar Mini-Map"), "", EA_MINIMAP, opened && t->minimap, opened && t->scrollbar);
        break;
    case SUB_CODE_FOLDING:
        ACTION(_("Fold Current Node"), "", EA_FOLD_CURRENT, 0, opened);
        ACTION(_("Unfold Current Node"), "", EA_UNFOLD_CURRENT, 0, opened);
        ACTION(_("Fold Toplevel Nodes"), "Ctrl+Shift+-", EA_FOLD_TOP, 0, opened);
        ACTION(_("Unfold Toplevel Nodes"), "Ctrl+Shift++", EA_UNFOLD_TOP, 0, opened);
        ACTION(_("Fold All Recursively"), "", EA_FOLD_ALL, 0, opened);
        ACTION(_("Unfold All Recursively"), "", EA_UNFOLD_ALL, 0, opened);
        break;
    case MENU_LSP: /* the LSP client plugin's menu: edit's language server menu */
        ACTION(_("Language Server..."), "Ctrl+Alt+K", EA_LANGUAGE, 0, opened);
        break;
    case MENU_TOOLS: /* katepart5ui.rc's Tools (Encoding); Indent, Unindent as the handbook's Tools */
        ACTION(_("Reopen with Encoding..."), "", EA_REOPEN_ENCODING, 0, opened && t->doc->path[0]);
        ACTION(_("Indent"), "Ctrl+I", EA_INDENT, 0, opened);
        ACTION(_("Unindent"), "Ctrl+Shift+I", EA_UNINDENT, 0, opened);
        break;
    case MENU_SESSIONS:
        ACTION(_("Manage Sessions..."), "", EA_SESSION_MANAGE, 0, 1);
        break;
    }
#undef SUBMENU
#undef ACTION
    return n;
}

static void editor_action(int action)
{
    struct tab *t = cur_tab();
    switch (action) {
    case EA_NEW: new_document(); break;
    case EA_OPEN: open_dialog(); break;
    case EA_SAVE: if (t) save(t->doc); break;
    case EA_SAVE_AS: if (t) save_as(t->doc); break;
    case EA_SAVE_ALL: save_all(); break;
    case EA_RELOAD:
        if (t && (!doc_modified(t->doc) || query_close(t->doc))) reload(t->doc);
        break;
    case EA_CLOSE: if (t) close_tab(active, spaces[active].cur); break;
    case EA_CLOSE_ALL:
        if (query_quit()) {
            for (int s = 0; s < nspaces; s++) while (spaces[s].ntabs) drop_tab(s, 0);
            while (ndocs) free_doc(docs[0]);
        }
        break;
    case EA_QUIT: if (session_quit()) quitting = 1; break;
    case EA_UNDO: if (t) undo_redo(active, t, 0); break;
    case EA_REDO: if (t) undo_redo(active, t, 1); break;
    case EA_CUT: if (t) copy(t, 1); break;
    case EA_COPY: if (t) copy(t, 0); break;
    case EA_PASTE: if (t) paste(active, t, 0); break;
    case EA_PASTE_SELECTION: if (t) paste(active, t, 1); break;
    case EA_OVERWRITE: if (t) t->doc->overwrite = !t->doc->overwrite; break;
    case EA_SELECT_ALL:
        if (t) mc_select_all(t);
        break;
    case EA_DESELECT: if (t) mc_deselect(t); break;
    case EA_SPLIT_V: split_direction(1); break;
    case EA_SPLIT_H: split_direction(0); break;
    case EA_TOGGLE_SPLIT: edit_layout_toggle(&view_layout, active); break;
    case EA_CLOSE_VIEW: drop_space(active); break;
    case EA_CLOSE_OTHER_VIEWS:
        while (nspaces > 1) drop_space(active == 0 ? nspaces - 1 : 0);
        break;
    case EA_NEXT_VIEW: active = (active + 1) % nspaces; follow_project(); break;
    case EA_PREV_VIEW: active = (active + nspaces - 1) % nspaces; follow_project(); break;
    case EA_SIDEBARS: toggle_sidebar(); break;
    case EA_PROJECTS: show_tool_view(SIDE_PROJECTS); break;
    case EA_WRAP:
        if (t) { t->wrap = !t->wrap; t->sub = t->left = 0; show_cursor(active, t, 0); }
        break;
    case EA_NUMBERS: if (t) { t->numbers = !t->numbers; show_cursor(active, t, 0); } break;
    case EA_ICON_BORDER: if (t) { t->icons = !t->icons; show_cursor(active, t, 0); } break;
    case EA_FIND: if (t) open_search(0); break;
    case EA_REPLACE: if (t) open_search(1); break;
    case EA_NEXT_MATCH: if (t) find_next(0); break;
    case EA_PREV_MATCH: if (t) find_next(1); break;
    case EA_FIND_SELECTED: if (t) find_selected(active, 0); break;
    case EA_FIND_SELECTED_BACK: if (t) find_selected(active, 1); break;
    case EA_GOTO: if (t) open_goto(); break;
    case EA_COMMENT: if (t) comment(active, t, COMMENT); break;
    case EA_UNCOMMENT: if (t) comment(active, t, UNCOMMENT); break;
    case EA_TOGGLE_COMMENT: if (t) comment(active, t, TOGGLE_COMMENT); break;
    case EA_INDENT: if (t) indent_selection(t, 1); break;
    case EA_UNINDENT: if (t) indent_selection(t, -1); break;
    case EA_MATCH_BRACKET: if (t) to_bracket(active, t, 0); break;
    case EA_SELECT_BRACKET: if (t) to_bracket(active, t, 1); break;
    case EA_READONLY: if (t) toggle_readonly(t->doc); break;
    case EA_REOPEN_ENCODING: if (t) reopen_encoding(t->doc); break;
    case EA_FONT_UP: font_zoom(1, 0); break;
    case EA_FONT_DOWN: font_zoom(-1, 0); break;
    case EA_FONT_RESET: font_zoom(0, 1); break;
    case EA_LANGUAGE: language_menu(); break;
    case EA_SESSION_MANAGE: session_menu(); break;
    case EA_ADD_ABOVE: if (t) mc_add_vertical(active, t, -1); break;
    case EA_ADD_BELOW: if (t) mc_add_vertical(active, t, 1); break;
    case EA_LINE_CARETS: if (t) mc_line_ends(active, t); break;
    case EA_NEXT_OCCURRENCE: if (t) mc_occurrence(active, t, 0); break;
    case EA_ALL_OCCURRENCES: if (t) mc_occurrence(active, t, 1); break;
    case EA_SKIP_OCCURRENCE: if (t) mc_skip(t); break;
    case EA_BLOCK: if (t) mc_toggle_block(t); break;
    case EA_REMOVE_EMPTY: if (t) mc_remove_empty(t); break;
    case EA_CLEAR_CARETS: if (t) mc_clear(t); break;
    case EA_FOLD_CURRENT: if (t) fold_current(t, 1, 0); break;
    case EA_UNFOLD_CURRENT: if (t) fold_current(t, 0, 0); break;
    case EA_FOLD_TOP: if (t) fold_top_level(t, 1, 0); break;
    case EA_UNFOLD_TOP: if (t) fold_top_level(t, 0, 0); break;
    case EA_FOLD_ALL: if (t) fold_top_level(t, 1, 1); break;
    case EA_UNFOLD_ALL: if (t) fold_top_level(t, 0, 1); break;
    case EA_FIND_FILES: filesearch_menu(); break;
    case EA_FILE_RESULTS: filesearch_results_ui(); break;
    case EA_QUICK_OPEN: quick_open(); break;
    case EA_LAST_USED:
    case EA_LAST_USED_REVERSE:
        switcher_menu(action == EA_LAST_USED_REVERSE);
        break;
    case EA_PREV_DOCUMENT: docs_step(-1); break;
    case EA_NEXT_DOCUMENT: docs_step(1); break;
    case EA_NEXT_TAB: tab_step(1); break;
    case EA_PREV_TAB: tab_step(-1); break;
    case EA_DOCUMENTS: show_tool_view(SIDE_DOCUMENTS); break;
    case EA_SCROLLBAR_MARKS: if (t) t->marks = !t->marks; break;
    case EA_MINIMAP:
        if (t) { t->minimap = !t->minimap; show_cursor(active, t, 0); }
        break;
    case EA_SET_BOOKMARK: if (t) bookmark_toggle(t); break;
    case EA_CLEAR_BOOKMARKS: if (t) bookmarks_clear(t->doc); break;
    case EA_PREV_BOOKMARK: if (t) bookmark_step(active, t, -1); break;
    case EA_NEXT_BOOKMARK: if (t) bookmark_step(active, t, 1); break;
    case EA_PREV_MODIFIED: if (t) modified_line_step(active, t, -1); break;
    case EA_NEXT_MODIFIED: if (t) modified_line_step(active, t, 1); break;
    default:
        if (t && action >= EA_BOOKMARK && (size_t)(action - EA_BOOKMARK) < ui_of(t->doc)->bookmarks.n) {
            bookmark_go(active, t, line_of(t->doc, ui_of(t->doc)->bookmarks.at[action - EA_BOOKMARK]));
        }
        break;
    }
}

static void draw_menubar(void)
{
    screen_fill(0, 1, screen_cols(), 1, C_WINDOW);
    int x = 1;
    for (int i = 0; i < EDITOR_MENUS; i++) {
        const char *title = editor_menu_title(i);
        int w = ui_cells(title) + 2;
        put_text(x, 1, screen_cols(), title, C_WTEXT, C_WINDOW);
        ui_hit(x - 1, 1, w, 1, HIT_MENU + i);
        x += w;
    }
    if (sidebar && SIDE_W > 2 && screen_rows() > 2) {
        screen_text(SIDE_W - 2, 2, "x", C_WTEXT, C_WINDOW);
        ui_hit(SIDE_W - 2, 2, 1, 1, HIT_TREE_CLOSE);
    }
}

/* Menu (or submenu) at (x, y); a submenu chosen opens at the right of its
 * item, as a cascading menu. */
static void editor_menu_at(int menu, int x, int y)
{
    struct editor_item items[EDITOR_ITEMS];
    int n = editor_items(menu, items);
    if (!n) return;
    const char *lines[EDITOR_ITEMS + 1];
    char text[EDITOR_ITEMS][160];
    int width = 0, wide = 0;
    for (int i = 0; i < n; i++) {
        int len = ui_cells(items[i].label);
        if (len > width) width = len;
    }
    for (int i = 0; i < n; i++) {
        char padded[128];
        snprintf(text[i], sizeof text[i], "%c %s  %s%s", items[i].checked ? '*' : ' ',
                 ui_pad(padded, sizeof padded, items[i].label, width),
                 items[i].shortcut, items[i].enabled ? "" : _(" (unavailable)"));
        lines[i] = text[i];
        int cells = ui_cells(text[i]);
        if (cells > wide) wide = cells;
    }
    lines[n] = NULL;
    int selected = ui_menu(x, y, lines);
    if (selected < 0 || selected >= n || !items[selected].enabled) return;
    if (items[selected].action >= EA_SUBMENU && items[selected].action < EA_BOOKMARK) {
        /* Where ui_menu put this one: kept on the screen. */
        int w = wide + 2, cols = screen_cols(), rows = screen_rows();
        x = x + w > cols ? cols - w : x;
        y = y + n > rows ? rows - n : y;
        editor_menu_at(items[selected].action - EA_SUBMENU, (x < 0 ? 0 : x) + w, (y < 0 ? 0 : y) + selected);
        return;
    }
    editor_action(items[selected].action);
}

static void editor_menu(int menu)
{
    int x = 1;
    for (int i = 0; i < menu && i < EDITOR_MENUS; i++) x += ui_cells(editor_menu_title(i)) + 2;
    editor_menu_at(menu, x, 2);
}
