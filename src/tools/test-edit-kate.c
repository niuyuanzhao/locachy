/*
 * Kate's visible behaviour in edit, driven through the real editor's keys,
 * pointer and drawing with the in-memory screen of test-edit-ui.c: Quick
 * Open, the Documents tool view, the Ctrl+Tab document switcher, the
 * scrollbar with its marks and the mini-map, bookmarks, Auto Brackets,
 * Remove Spaces on save, and the menus, splits and font zoom.
 *   test-edit-kate DIR   (built by test-edit-kate.sh)
 */
#ifndef EDIT_KATE_HARNESS
#error Build with test-edit-kate.sh
#endif
#include "base.h"
#include "i18n-stub.h"
#include EDIT_KATE_HARNESS
#include <pwd.h>

/* The user's home for the sessions (root runs this): a folder of the
 * fixtures, never the account's own (built with --wrap=getpwuid). */
static char kate_test_home[PATH_MAX];
struct passwd *__real_getpwuid(uid_t uid);
struct passwd *__wrap_getpwuid(uid_t uid)
{
    struct passwd *pw = __real_getpwuid(uid);
    if (!pw || uid != EDIT_USER || !*kate_test_home) return pw;
    static struct passwd copy;
    copy = *pw;
    copy.pw_dir = kate_test_home;
    return &copy;
}

/* A pointer event queued for a modal loop (a menu, Quick Open). */
static void push_pointer(int col, int row, int button, int pressed, int clicks)
{
    queue[qtail++ % 256] = (struct event){ K_POINTER, 0, NULL,
                                           { .x = col * 10 + 2, .y = row * 20 + 5, .col = col, .row = row,
                                             .button = button, .pressed = pressed, .clicks = clicks,
                                             .buttons = pressed && button == BTN_LEFT } };
}

static void push_text(const char *s)
{
    for (const char *p = s; *p; p++) push((unsigned char)*p, 0, NULL);
}

static void pointer_at(int col, int row, int button, int pressed, int clicks, int buttons, int wheel, int mods)
{
    ptr_now = (struct pointer_event){ .x = col * 10 + 2, .y = row * 20 + 5, .col = col, .row = row, .button = button,
                                      .pressed = pressed, .clicks = clicks, .buttons = buttons, .wheel = wheel };
    mods_now = mods;
    key_event(K_POINTER);
    mods_now = 0;
}

/* The first row from start that has s, or -1. */
static int row_from(int start, const char *s)
{
    for (int r = start; r < ROWS; r++) {
        char line[COLS * 4 + 1];
        row_text(r, line, sizeof line);
        if (strstr(line, s)) return r;
    }
    return -1;
}

static int row_has(int row, const char *s)
{
    char line[COLS * 4 + 1];
    row_text(row, line, sizeof line);
    return strstr(line, s) != NULL;
}

/* The column of s in row, or -1. */
static int col_of(int row, const char *s)
{
    char line[COLS * 4 + 1];
    row_text(row, line, sizeof line);
    char *p = strstr(line, s);
    return p ? (int)(p - line) : -1;
}

static void make_dir(const char *rel)
{
    CHECK(mkdir(at_dir(rel), 0755) == 0);
    if (is_root) CHECK(chown(at_dir(rel), EDIT_USER, EDIT_USER) == 0);
}

static const char *path_of(void)
{
    return cur_tab() ? cur_tab()->doc->path : "";
}

/* The list has the file rel (of the fixtures). */
static int qo_lists(const char *rel)
{
    for (int i = 0; i < qo.n; i++) {
        if (strcmp(qo.e[i].path, at_dir(rel)) == 0) return 1;
    }
    return 0;
}

static void test_quick_open(void)
{
    reset();
    char cmd[4096];
    /* git runs as root here, in folders that are root's until it is done. */
    CHECK(mkdir(at_dir("qo"), 0755) == 0 && mkdir(at_dir("qo/src"), 0755) == 0 && mkdir(at_dir("qo/doc"), 0755) == 0);
    put_file(at_dir("qo/README.md"), "readme\n");
    put_file(at_dir("qo/src/main.c"), "int main;\nint two;\nint three;\nint four;\n");
    put_file(at_dir("qo/src/util.c"), "int util;\n");
    put_file(at_dir("qo/doc/index.docbook"), "<book/>\n");
    put_file(at_dir("qo/.gitignore"), "*.o\n");
    put_file(at_dir("qo/build.o"), "obj\n");
    snprintf(cmd, sizeof cmd, "cd %s && git init -q && git add README.md src doc .gitignore && "
             "git -c user.name=t -c user.email=t@t commit -qm one%s%s", at_dir("qo"),
             is_root ? " && chown -R 1000:1000 " : "", is_root ? at_dir("qo") : "");
    CHECK(system(cmd) == 0);
    CHECK(open_file(at_dir("qo/src/main.c")));
    CHECK(open_file(at_dir("qo/README.md")));
    draw();

    /* Its list: the open documents (last used first), then the project's
     * files as the Projects tree lists them (git's: tracked and new, not
     * ignored), each once. */
    qo_build();
    for (int i = 0; qo.n != 5 && i < qo.n; i++) printf("  entry %d: %s%s\n", i, qo.e[i].path, qo.e[i].doc ? " (open)" : "");
    CHECK(qo.n == 5 && qo.e[0].doc && strcmp(qo.e[0].name, "README.md") == 0 && qo.e[1].doc &&
          strcmp(qo.e[1].name, "main.c") == 0 && !qo.e[2].doc);
    /* The tree's order (folders first), its paths shown without the project's folder. */
    CHECK(strcmp(qo.e[2].rel, "doc/index.docbook") == 0 && strcmp(qo.e[3].rel, "src/util.c") == 0 &&
          strcmp(qo.e[4].rel, ".gitignore") == 0 && !qo_lists("qo/build.o"));
    /* The handbook's matching: the file name only, the path with a "/". */
    qo_filter("src"); /* a folder's name: no file is named so */
    for (int i = 0; i < qo.nshown; i++) CHECK(!strstr(qo.e[qo.shown[i]].path, "/src/"));
    qo_filter("doc/index");
    CHECK(qo.nshown == 1 && strcmp(qo.e[qo.shown[0]].name, "index.docbook") == 0);
    qo_filter("ul");
    CHECK(qo.nshown == 1 && strcmp(qo.e[qo.shown[0]].name, "util.c") == 0);
    qo_filter("src/");
    CHECK(qo.nshown == 2);
    int score, m[FUZZY_MAX], nm;
    CHECK(fuzzy_match("mc", "main.c", &score, m, &nm) && nm == 2 && m[0] == 0 && m[1] == 5);
    CHECK(!fuzzy_match("xyz", "main.c", &score, NULL, NULL));
    qo_free();
    puts("PASS Quick Open's list: open documents last used first, then the project's files as the tree lists them "
         "(git's, ignored ones left out); kfts fuzzy matching of the name, of the path with a \"/\"");

    /* Ctrl+Alt+O: the previous document selected at once, Enter shows it. */
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/main.c")) == 0);
    /* Typing filters (the frame drawn before Enter still shows the list). */
    push_text("util");
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/util.c")) == 0);
    CHECK(row_from(3, "util.c src") == 3 && row_from(3, "README.md") < 0); /* the list under the text typed */
    CHECK(ndocs == 3);
    /* Esc closes it, nothing opened. */
    push_text("index");
    push(27, 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/util.c")) == 0 && ndocs == 3);
    /* Up, Down, Page Down and Page Up move in the list. */
    push(K_UP, 0, NULL);
    push(K_DOWN, 0, NULL);
    push(K_DOWN, 0, NULL);
    push(K_UP, 0, NULL);
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/main.c")) == 0); /* main.c, util.c, main.c */
    push(K_PGDN, 0, NULL);
    push(K_PGUP, 0, NULL);
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/main.c")) == 0); /* the first: the active one */
    /* Home and End are the text field's (Kate forwards only Up, Down, Page Up
     * and Page Down to the list): "ain", Home, "m", End, ":3" is main:3, and
     * FILE:LINE goes to the line. */
    push_text("ain");
    push(K_BOL, 0, NULL);
    push('m', 0, NULL);
    push(K_EOL, 0, NULL);
    push_text(":3");
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/main.c")) == 0 && line_of(cur_tab()->doc, cur_tab()->cursor) == 2);
    /* Home in a list of several: the field's, the selection stays (Enter on
     * the previous document, util.c, not the list's first). */
    push(K_BOL, 0, NULL);
    push(K_EOL, 0, NULL);
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/src/util.c")) == 0);
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT); /* back to main.c */
    CHECK(strcmp(path_of(), at_dir("qo/src/main.c")) == 0);
    /* A click on a row opens its file. */
    struct qo_box b;
    qo_geometry(&b);
    push_text("index");
    push_pointer(b.x + 3, b.y + 1, BTN_LEFT, 1, 1);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/doc/index.docbook")) == 0);
    /* From the menu: Alt+G, Go > Quick Open (kateui.rc: its first item). */
    push('\n', 0, NULL);
    push_text("readme");
    push('\n', 0, NULL);
    key('g', KEYS_ALT);
    CHECK(qhead == qtail && strcmp(path_of(), at_dir("qo/README.md")) == 0);
    puts("PASS Quick Open (Ctrl+Alt+O, Go menu): the previous document first, typing filters, Up/Down/PgUp/PgDn in "
         "the list, Home/End in the text field, Enter and a click open in the active view, FILE:LINE, Esc cancels");

    /* Outside version control: the file's folder, hidden files left out. */
    make_dir("qo-plain");
    make_dir("qo-plain/sub");
    put_file(at_dir("qo-plain/a.txt"), "a\n");
    put_file(at_dir("qo-plain/.hidden"), "h\n");
    put_file(at_dir("qo-plain/sub/b.txt"), "b\n");
    CHECK(open_file(at_dir("qo-plain/a.txt")));
    qo_build();
    CHECK(qo_lists("qo-plain/sub/b.txt") && !qo_lists("qo-plain/.hidden"));
    qo_free();
    push_text("b.txt");
    push('\n', 0, NULL);
    key('o', KEYS_CTRL | KEYS_ALT);
    CHECK(strcmp(path_of(), at_dir("qo-plain/sub/b.txt")) == 0);
    puts("PASS Quick Open without version control: the file's folder and its folders, hidden files left out");
}

static void test_documents(void)
{
    reset();
    make_dir("docs");
    make_dir("docs/a");
    make_dir("docs/a/sub");
    make_dir("docs/b");
    put_file(at_dir("docs/a/x.txt"), "x\n");
    put_file(at_dir("docs/a/sub/y.txt"), "y\n");
    put_file(at_dir("docs/b/z.txt"), "z\n");
    CHECK(open_file(at_dir("docs/a/sub/y.txt")));
    CHECK(open_file(at_dir("docs/a/x.txt")));
    key('n', KEYS_CTRL);
    CHECK(open_file(at_dir("docs/b/z.txt")));
    editor_action(EA_DOCUMENTS); /* View > Tool Views > Documents */
    CHECK(sidebar && side_view == SIDE_DOCUMENTS && !tree_focus);
    draw();
    /* At the left of the editing area, its toolbar, Tree Mode: the folders
     * (a, b), then the document without a file; z.txt's folder opened. */
    CHECK(row_has(2, "Documents") && grid[2][1].bg == C_BORDER && row_has(3, "New") && row_has(3, "Save"));
    CHECK(row_has(4, "+ a") && row_has(5, "- b") && row_has(6, "z.txt") && row_has(7, "Untitled"));
    CHECK(col_of(6, "z.txt") == 5 && grid[6][1].bg == C_FOCUS && col_of(4, "+ a") == 1 && grid[4][40].cp != '+');
    /* A click on a folder's mark opens it; on a document shows it. */
    click(1, 4, BTN_LEFT, 1, 1);
    draw();
    CHECK(row_has(4, "- a") && row_has(5, "+ sub") && row_has(6, "x.txt") && row_has(7, "- b"));
    click(8, 6, BTN_LEFT, 1, 1);
    CHECK(strcmp(path_of(), at_dir("docs/a/x.txt")) == 0 && !tree_focus);
    /* A modified document has its mark. */
    keys("q");
    draw();
    CHECK(row_has(6, "* x.txt") && grid[6][1].bg == C_FOCUS);
    /* Alt+Down, Alt+Up: the next and previous documents in the list. */
    key(K_DOWN, KEYS_ALT);
    CHECK(strcmp(path_of(), at_dir("docs/b/z.txt")) == 0);
    key(K_DOWN, KEYS_ALT);
    CHECK(!cur_tab()->doc->path[0]); /* Untitled */
    key(K_DOWN, KEYS_ALT);
    CHECK(strcmp(path_of(), at_dir("docs/a/sub/y.txt")) == 0); /* round */
    key(K_UP, KEYS_ALT);
    CHECK(!cur_tab()->doc->path[0]);
    draw();
    /* The toolbar's Previous Document. */
    click(col_of(3, "Prev"), 3, BTN_LEFT, 1, 1);
    CHECK(strcmp(path_of(), at_dir("docs/b/z.txt")) == 0);
    draw();
    /* The context menu: Close (a modified one asks: Discard). */
    int row = on_screen("* x.txt");
    CHECK(row > 3);
    push(K_DOWN, 0, NULL);
    push(K_DOWN, 0, NULL);
    push(K_DOWN, 0, NULL);
    push('\n', 0, NULL);
    push('d', 0, NULL);
    click(8, row, BTN_RIGHT, 1, 1);
    CHECK(qhead == qtail && ndocs == 3 && !doc_for_path(at_dir("docs/a/x.txt")));
    draw();
    CHECK(on_screen("x.txt") < 0);
    /* List Mode, from the context menu of its empty part. */
    push(K_DOWN, 0, NULL);
    push('\n', 0, NULL);
    click(5, ROWS - 2, BTN_RIGHT, 1, 1);
    CHECK(docs_list_mode);
    draw();
    CHECK(row_has(4, "Untitled") && row_has(5, "y.txt") && row_has(6, "z.txt") && !row_has(5, "sub"));
    docs_list_mode = 0;
    /* The sidebar's buttons switch to Projects and back. */
    click(col_of(2, "Projects"), 2, BTN_LEFT, 1, 1);
    CHECK(side_view == SIDE_PROJECTS && tree_focus);
    draw();
    CHECK(grid[2][1].bg != C_BORDER);
    click(2, 2, BTN_LEFT, 1, 1);
    CHECK(side_view == SIDE_DOCUMENTS && !tree_focus);
    puts("PASS Documents tool view: left of the editing area, toolbar, Tree Mode with the active document's folders "
         "open, a click shows a document or opens a folder, the modified mark, Alt+Up/Alt+Down, Close from the "
         "context menu, List Mode, its sidebar button");
}

static void release_ctrl(void)
{
    mods_now = 0;
    step(); /* the loop's poll ends with no key: Ctrl is up */
}

static void test_switcher(void)
{
    reset();
    struct doc *a = open_text("sw-a.txt", "a\n")->doc;
    struct doc *b = open_text("sw-b.txt", "b\n")->doc;
    struct doc *c = open_text("sw-c.txt", "c\n")->doc;
    /* Ctrl+Tab: the last used ones first, the previous selected. */
    key('\t', KEYS_CTRL);
    CHECK(switcher_open() && doc_switcher.n == 3 && doc_switcher.list[0] == c && doc_switcher.list[1] == b &&
          doc_switcher.sel == 1);
    draw();
    int row = row_from(4, "sw-b.txt"); /* below the tab bar: the list */
    CHECK(row >= 4 && grid[row][col_of(row, "sw-b.txt")].bg == C_FOCUS && row_from(4, "sw-c.txt") == row - 1);
    release_ctrl();
    CHECK(!switcher_open() && cur_tab()->doc == b);
    /* Tab again with Ctrl held: on, round. */
    key('\t', KEYS_CTRL);
    key('\t', KEYS_CTRL);
    CHECK(doc_switcher.sel == 2);
    key('\t', KEYS_CTRL);
    CHECK(doc_switcher.sel == 0);
    key('\t', KEYS_CTRL | KEYS_SHIFT);
    CHECK(doc_switcher.sel == 2);
    release_ctrl();
    CHECK(cur_tab()->doc == a);
    /* Ctrl+Shift+Tab opens at the one before the last. */
    key('\t', KEYS_CTRL | KEYS_SHIFT);
    CHECK(doc_switcher.sel == 1);
    /* Up, Down, Home, End; a letter: the next whose name starts so. */
    key(K_EOL, KEYS_CTRL);
    CHECK(doc_switcher.sel == 2);
    key(K_BOL, KEYS_CTRL);
    CHECK(doc_switcher.sel == 0);
    key(K_DOWN, KEYS_CTRL);
    CHECK(doc_switcher.sel == 1);
    key(K_UP, KEYS_CTRL);
    CHECK(doc_switcher.sel == 0);
    key('s', KEYS_CTRL);
    CHECK(doc_switcher.sel == 1);
    /* Esc: nothing switched. */
    key(27, KEYS_CTRL);
    CHECK(!switcher_open() && cur_tab()->doc == a);
    release_ctrl();
    CHECK(cur_tab()->doc == a);
    /* Ctrl+W closes the selected document; the list stays. */
    key('\t', KEYS_CTRL);
    CHECK(doc_switcher.list[doc_switcher.sel] == b);
    key('w', KEYS_CTRL);
    CHECK(ndocs == 2 && switcher_open() && doc_switcher.n == 2 && !doc_for_path(at_dir("sw-b.txt")));
    release_ctrl();
    CHECK(cur_tab()->doc == c);
    /* A click on a row switches to it. */
    key('\t', KEYS_CTRL);
    draw();
    row = row_from(4, "sw-c.txt");
    CHECK(row >= 4);
    pointer_at(col_of(row, "sw-c.txt"), row, BTN_LEFT, 1, 1, 1, 0, KEYS_CTRL);
    CHECK(!switcher_open() && cur_tab()->doc == c);
    mods_now = 0;
    /* A key with Ctrl released switches first, then does its own. */
    key('\t', KEYS_CTRL);
    key(K_EOL, 0);
    CHECK(!switcher_open() && cur_tab()->doc == a && cur_tab()->cursor == 1);
    /* From View > Last Used Views: no Ctrl to release; Enter switches. */
    editor_action(EA_LAST_USED);
    CHECK(switcher_open() && doc_switcher.sel == 1);
    release_ctrl();
    CHECK(switcher_open());
    key('\n', 0);
    CHECK(!switcher_open() && cur_tab()->doc == c);
    puts("PASS Ctrl+Tab document switcher (Kate's Document Switcher plugin): last used first, Tab and Shift+Tab "
         "round with Ctrl held, Ctrl+Shift+Tab, Up/Down/Home/End, a letter, Esc, Ctrl+W closes, Ctrl's release or "
         "a click switches");
}

static void test_scrollbar(void)
{
    reset();
    /* The program's defaults (kateconfig.cpp): scrollbar, mini-map, no marks. */
    CHECK(edit_defaults_scrollbar == 1 && edit_defaults_minimap == 1 && edit_defaults_marks == 0);
    view_scrollbar_default = edit_defaults_scrollbar;
    view_minimap_default = edit_defaults_minimap;
    view_marks_default = edit_defaults_marks;
    char text[64 * 301] = "";
    for (int i = 0; i < 300; i++) {
        char line[64];
        snprintf(line, sizeof line, i % 2 ? "    int value%d;\n" : "\n", i);
        strcat(text, line);
    }
    struct tab *t = open_text("scroll.c", text);
    struct doc *d = t->doc;
    CHECK(t->scrollbar && t->minimap && !t->marks && minimap_cols() == 6);
    CHECK(text_width(0, t) == COLS - 6 - gutter(t));
    draw();
    struct scroll_geom g;
    scroll_geometry(0, t, &g);
    CHECK(g.x == COLS - 6 && g.y == 3 && g.h == 26 && g.total == 301 && g.doc_rows == 26 && g.slider_a == 0);
    /* The mini-map: the visible part highlighted, the rest faded; text as
     * the colour of its characters. */
    uint32_t slider = colour_mix(C_BG, C_FOCUS, 50);
    CHECK(grid[g.y][g.x + 5].bg == slider || grid[g.y][g.x + 5].bg != C_BG);
    CHECK(grid[g.y + 10][g.x + 1].bg != C_BG && grid[g.y + 10][g.x + 5].bg == C_BG);
    CHECK(ui_hit_at(g.x + 2, g.y + 4) == HIT_SCROLL);
    /* The line modification markers: modified, then saved. */
    t->cursor = t->anchor = line_end(d, 101);
    keys("x");
    draw();
    int mrow = (int)(101 * 26 / 301);
    CHECK(grid[g.y + mrow][g.x].bg == C_MODIFIED && grid[g.y + mrow + 3][g.x].bg == C_BG);
    key('s', KEYS_CTRL);
    draw();
    CHECK(grid[g.y + mrow][g.x].bg == C_SAVED);
    /* A click jumps there, the line in the middle; the cursor stays. */
    size_t cursor = t->cursor;
    click(g.x + 2, g.y + 13, BTN_LEFT, 1, 1);
    click(g.x + 2, g.y + 13, BTN_LEFT, 0, 0);
    CHECK(line_of(d, t->top) == (size_t)(13.5 / 26 * 301) - 13 && t->cursor == cursor);
    draw();
    scroll_geometry(0, t, &g);
    CHECK(g.slider_a > 10 && g.slider_a < 15);
    /* Dragging the slider moves the view with it. */
    size_t before = line_of(d, t->top);
    pointer_at(g.x + 2, g.y + g.slider_a, BTN_LEFT, 1, 1, 1, 0, 0);
    pointer_at(g.x + 2, g.y + g.slider_a + 5, 0, 0, 0, 1, 0, 0);
    pointer_at(g.x + 2, g.y + g.slider_a + 5, BTN_LEFT, 0, 0, 0, 0, 0);
    CHECK(line_of(d, t->top) > before + 50 && !scroll_drag.on && t->cursor == cursor);
    puts("PASS mini-map (Kate's default, 60 pixels): the document's text in its colours, the visible part "
         "highlighted, modified and saved lines in its margin, a click jumps there, the slider drags the view");

    /* Bookmarks: Ctrl+B, Alt+PgDown, Alt+PgUp round; their lines tinted. */
    t->cursor = t->anchor = line_start(d, 200);
    key('b', KEYS_CTRL);
    t->cursor = t->anchor = line_start(d, 51) + 2;
    key('b', KEYS_CTRL);
    CHECK(bookmarked(d, 200) && bookmarked(d, 51) && ui_of(d)->bookmarks.n == 2);
    key(K_PGDN, KEYS_ALT);
    CHECK(t->cursor == line_start(d, 200));
    key(K_PGDN, KEYS_ALT);
    CHECK(t->cursor == line_start(d, 51));
    key(K_PGUP, KEYS_ALT);
    CHECK(t->cursor == line_start(d, 200));
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    int brow = -1;
    for (int r = 0; r < h; r++) {
        if (grid[y0 + r][gutter(t) + 2].bg == colour_mix(C_LINE, C_BOOKMARK, 26)) brow = r;
    }
    CHECK(brow >= 0);
    /* The marks move with their lines. */
    t->cursor = t->anchor = 0;
    keys("\n\n");
    CHECK(bookmarked(d, 202) && bookmarked(d, 53) && !bookmarked(d, 200));
    key('z', KEYS_CTRL);
    key('z', KEYS_CTRL);
    CHECK(bookmarked(d, 200) && bookmarked(d, 51) && ui_of(d)->bookmarks.n == 2);
    /* Off by default, Show Scrollbar Marks shows them: bookmarks and Find
     * All's matches. */
    int bmark = (int)((26 - 1) * 200.0 / 300 + 0.5);
    draw();
    CHECK(grid[g.y + bmark][g.x + 2].bg != C_BOOKMARK);
    editor_action(EA_SCROLLBAR_MARKS);
    CHECK(t->marks);
    draw();
    CHECK(grid[g.y + bmark][g.x + 2].bg == C_BOOKMARK);
    key('r', KEYS_CTRL);
    key('a', KEYS_CTRL);
    keys("value299");
    key('f', KEYS_ALT); /* Find All */
    CHECK(ui_of(d)->nhl == 1);
    draw();
    scroll_geometry(0, t, &g);
    int frow = (int)((g.doc_rows - 1) * 299.0 / 300 + 0.5);
    CHECK(grid[g.y + frow][g.x + 2].bg == C_TEXT);
    key(27, 0); /* the bar closed, its highlights with it */
    CHECK(ui_of(d)->nhl == 0);
    puts("PASS bookmarks (Ctrl+B, Alt+PgDown, Alt+PgUp round, their lines tinted, moving with the text) and "
         "Show Scrollbar Marks (off by default): bookmarks and Find All's matches on the mini-map");

    /* Without the mini-map: the scrollbar's column, its slider and marks. */
    editor_action(EA_MINIMAP);
    CHECK(!t->minimap && scroll_cols(0, t) == 1 && text_width(0, t) == COLS - 1 - gutter(t));
    draw();
    scroll_geometry(0, t, &g);
    CHECK(g.x == COLS - 1 && grid[g.y + g.slider_a][g.x].bg == C_SLIDER);
    CHECK(grid[g.y + bmark][g.x].bg == C_BOOKMARK || grid[g.y + bmark][g.x].cp == '-');
    click(g.x, g.y + 20, BTN_LEFT, 1, 1);
    click(g.x, g.y + 20, BTN_LEFT, 0, 0);
    CHECK(line_of(d, t->top) == (size_t)(20.5 / 26 * 301) - 13);
    /* The middle button jumps too. */
    click(g.x, g.y + 2, BTN_MIDDLE, 1, 1);
    click(g.x, g.y + 2, BTN_MIDDLE, 0, 0);
    CHECK(line_of(d, t->top) < 20);
    draw();
    scroll_geometry(0, t, &g);
    before = line_of(d, t->top);
    pointer_at(g.x, g.y + g.slider_a, BTN_LEFT, 1, 1, 1, 0, 0);
    pointer_at(g.x, g.y + g.slider_a + 10, 0, 0, 0, 1, 0, 0);
    pointer_at(g.x, g.y + g.slider_a + 10, BTN_LEFT, 0, 0, 0, 0, 0);
    CHECK(line_of(d, t->top) >= before + 100);
    editor_action(EA_MINIMAP);
    puts("PASS the scrollbar without the mini-map: one column, its slider and marks, a click and the middle "
         "button jump, the slider drags");

    /* A large document with the mini-map shown (the X230's default). */
    reset();
    const char *path = at_dir("big-minimap.c");
    FILE *f = fopen(path, "w");
    CHECK(f);
    for (int i = 0; i < 1000000; i++) fprintf(f, "    int line%07d = %d; /* comment */\n", i, i);
    CHECK(fclose(f) == 0);
    if (is_root) CHECK(chown(path, EDIT_USER, EDIT_USER) == 0);
    double t0 = ms();
    CHECK(open_file(path));
    draw();
    double open_ms = ms() - t0;
    t = cur_tab();
    d = t->doc;
    t0 = ms();
    key(K_EOL, KEYS_CTRL);
    draw();
    double end_ms = ms() - t0;
    scroll_geometry(0, t, &g);
    CHECK(g.total == 1000001 && g.slider_b == g.h);
    t0 = ms();
    click(g.x + 2, g.y, BTN_LEFT, 1, 1);
    click(g.x + 2, g.y, BTN_LEFT, 0, 0);
    /* The first row holds 1000001 / 26 lines: the view at its middle. */
    CHECK(line_of(d, t->top) == (size_t)(0.5 / 26 * 1000001) - 13 && line_of(d, t->cursor) == 1000000);
    keys("x");
    draw();
    double click_ms = ms() - t0;
    CHECK(line_of(d, t->cursor) == 1000000 && line_of(d, t->top) > 999000);
    printf("  1000000 lines with the mini-map: open and draw %.0f ms, Ctrl+End %.0f ms, a click and an edit %.0f ms\n",
           open_ms, end_ms, click_ms);
    CHECK(end_ms < 2000 && click_ms < 2000);
    view_scrollbar_default = view_minimap_default = 0;
    puts("PASS the mini-map of a million lines: drawn, jumped and edited (the times above)");
}

static void test_auto_brackets(void)
{
    reset();
    struct tab *t = open_text("brackets.txt", "abc\n");
    struct doc *d = t->doc;
    t->cursor = t->anchor = 3;
    keys("(");
    CHECK(text_is(d, "abc()\n") && t->cursor == 4);
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "abc\n")); /* the pair is one undo step */
    t->cursor = t->anchor = 3;
    keys("(");
    keys("x");
    CHECK(text_is(d, "abc(x)\n") && t->cursor == 5);
    keys(")");
    CHECK(text_is(d, "abc(x)\n") && t->cursor == 6); /* typed over */
    key('z', KEYS_CTRL);
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "abc\n"));
    t->cursor = t->anchor = 3;
    keys("[");
    CHECK(text_is(d, "abc[]\n"));
    key('\b', 0);
    CHECK(text_is(d, "abc\n") && t->cursor == 3); /* both */
    t->cursor = t->anchor = 0;
    keys("{");
    CHECK(text_is(d, "{abc\n")); /* a letter after it: none */
    t->cursor = t->anchor = line_end(d, 0);
    keys(" don't");
    CHECK(text_is(d, "{abc don't\n")); /* text without a language: an apostrophe */
    keys(" \"q");
    CHECK(text_is(d, "{abc don't \"q\"\n"));
    keys("\"");
    CHECK(text_is(d, "{abc don't \"q\"\n") && t->cursor == line_end(d, 0));
    /* A selection is enclosed and stays selected. */
    t->anchor = 1;
    t->cursor = 4;
    keys("(");
    CHECK(text_is(d, "{(abc) don't \"q\"\n") && t->anchor == 2 && t->cursor == 5);
    keys("<");
    CHECK(text_is(d, "{(<abc>) don't \"q\"\n") && t->anchor == 3 && t->cursor == 6);
    keys("\"");
    CHECK(text_is(d, "{(<\"abc\">) don't \"q\"\n"));
    reset();
    t = open_text("brackets.c", "int c = ;\n");
    d = t->doc;
    syntax_parse(d->syntax, &d->buf, 0);
    t->cursor = t->anchor = 8;
    keys("'");
    CHECK(text_is(d, "int c = '';\n") && t->cursor == 9); /* code: the quote pair */
    keys("x'");
    CHECK(text_is(d, "int c = 'x';\n") && t->cursor == 11);
    puts("PASS Auto Brackets (on by default): ( [ { \" and, in code, ' closed; typed over; Backspace takes the pair; "
         "none before a letter; an apostrophe in text; a selection enclosed in <>(){}[]'\"; one undo step");
}

static void test_remove_spaces(void)
{
    reset();
    CHECK(REMOVE_SPACES == 1); /* Remove Spaces: on modified lines, Kate's default */
    struct tab *t = open_text("spaces.txt", "keep   \nedit\nlast  \n");
    struct doc *d = t->doc;
    t->cursor = t->anchor = line_end(d, 1);
    keys("x   ");
    key('s', KEYS_CTRL);
    CHECK(strcmp(file_bytes(at_dir("spaces.txt")), "keep   \neditx\nlast  \n") == 0);
    CHECK(text_is(d, "keep   \neditx\nlast  \n") && !doc_modified(d));
    /* Saved lines stay cleaned; untouched ones keep their spaces. */
    t->cursor = t->anchor = line_end(d, 2);
    keys(" \t");
    t->cursor = t->anchor = line_end(d, 1);
    keys("  ");
    key('s', KEYS_CTRL);
    CHECK(strcmp(file_bytes(at_dir("spaces.txt")), "keep   \neditx\nlast\n") == 0);
    /* Undone back to the file as read: nothing is modified, nothing changes. */
    reset();
    t = open_text("spaces2.txt", "a  \nb\n");
    d = t->doc;
    t->cursor = t->anchor = 0;
    keys("y");
    key('z', KEYS_CTRL);
    CHECK(!doc_modified(d));
    key('s', KEYS_CTRL);
    CHECK(strcmp(file_bytes(at_dir("spaces2.txt")), "a  \nb\n") == 0);
    keys("z");
    key('s', KEYS_CTRL);
    CHECK(strcmp(file_bytes(at_dir("spaces2.txt")), "za\nb\n") == 0);
    puts("PASS Remove Spaces on save (on modified lines, the default): touched and saved lines lose their trailing "
         "spaces, the others keep them; undo back to the file leaves it");
}

static void test_menus_views_zoom(void)
{
    reset();
    struct tab *t = open_text("menus.md", "# Head\n\n*em* **strong** <https://example.org>\n");
    struct doc *d = t->doc;
    /* The menu bar as Kate's (kateui.rc with katepart5ui.rc and the default
     * plugins' merged in), Alt+letter opens each menu (Kate's mnemonics),
     * Esc closes it. */
    draw();
    CHECK(row_has(1, " File  Edit  Selection  Go  View  LSP Client  Tools  Sessions "));
    CHECK(strcmp(EDITOR_MNEMONICS, "fecgvlti") == 0 && EDITOR_MENUS == 8);
    const char *items[] = { "Save As...", "Paste Selection", "Select All", "Quick Open", "Split View",
                            "Language Server...", "Reopen with Encoding", "Manage Sessions" };
    for (int i = 0; i < EDITOR_MENUS; i++) {
        push(27, 0, NULL);
        key(EDITOR_MNEMONICS[i], KEYS_ALT);
        CHECK(qhead == qtail && on_screen(items[i]) >= 2);
        draw();
        CHECK(on_screen(items[i]) < 0);
    }
    push('\n', 0, NULL);
    key('f', KEYS_ALT); /* File > New */
    CHECK(ndocs == 2 && !cur_tab()->doc->path[0]);
    /* Shift+F10 opens the menu (File). */
    push(K_DOWN, 0, NULL);
    push(27, 0, NULL);
    key(K_F1 + 9, KEYS_SHIFT);
    CHECK(qhead == qtail && on_screen("Open...") == 3);
    puts("PASS menus: the menu bar File, Edit, Selection, Go, View, LSP Client, Tools, Sessions; Alt+F/E/C/G/V/L/T/I "
         "open them, Enter runs an item, Shift+F10 opens the menu bar's first");

    /* Each item under Kate's menu: find and replace under Edit, Go to Line,
     * brackets, modified lines and Bookmarks under Go, the borders under
     * View > Borders; every action edit has is in a menu, its shortcut kept. */
    reset();
    t = open_text("reach.txt", "one\ntwo\nthree\nfour\n");
    d = t->doc;
    key('n', KEYS_CTRL);
    show_doc(active, d);
    bookmark_set(d, 0, 1);
    bookmark_set(d, 3, 1);
    t = cur_tab();
    t->cursor = t->anchor = line_start(d, 1);
    struct { int menu, action; const char *label, *keys; } where[] = {
        { MENU_EDIT, EA_FIND, "Find...", "Ctrl+F" }, { MENU_EDIT, EA_REPLACE, "Replace...", "Ctrl+R" },
        { SUB_FIND_VARIANTS, EA_NEXT_MATCH, "Find Next", "F3" }, { MENU_EDIT, EA_FIND_FILES, "Find in Files...", "Ctrl+Shift+F" },
        { MENU_EDIT, EA_READONLY, "Read Only Mode", "" }, { MENU_SELECTION, EA_TOGGLE_COMMENT, "Toggle Comment", "Ctrl+/" },
        { MENU_SELECTION, EA_ADD_ABOVE, "Add Caret above Cursor", "Ctrl+Alt+Up" },
        { MENU_GO, EA_GOTO, "Go to Line...", "Ctrl+G" }, { MENU_GO, EA_MATCH_BRACKET, "Go to Matching Bracket", "Ctrl+6" },
        { MENU_GO, EA_PREV_MODIFIED, "Go to Previous Modified Line", "" },
        { MENU_GO, EA_NEXT_MODIFIED, "Go to Next Modified Line", "" },
        { MENU_GO, EA_QUICK_OPEN, "Quick Open", "Ctrl+Alt+O" }, { MENU_GO, EA_NEXT_DOCUMENT, "Next Document", "Alt+Down" },
        { SUB_BOOKMARKS, EA_SET_BOOKMARK, "Toggle Bookmark", "Ctrl+B" },
        { SUB_BOOKMARKS, EA_NEXT_BOOKMARK, "Next: 4 - \"four\"", "Alt+PgDown" },
        { SUB_BOOKMARKS, EA_PREV_BOOKMARK, "Previous: 1 - \"one\"", "Alt+PgUp" },
        { SUB_SPLIT_VIEW, EA_SPLIT_V, "Split Vertical", "Ctrl+Shift+L" },
        { SUB_TOOL_VIEWS, EA_SIDEBARS, "Show Sidebars", "Ctrl+Alt+Shift+F" },
        { SUB_BORDERS, EA_ICON_BORDER, "Show Icon Border", "F6" }, { SUB_BORDERS, EA_NUMBERS, "Show Line Numbers", "F11" },
        { SUB_WORD_WRAP, EA_WRAP, "Dynamic Word Wrap", "F10" }, { MENU_TOOLS, EA_INDENT, "Indent", "Ctrl+I" },
        { MENU_LSP, EA_LANGUAGE, "Language Server...", "Ctrl+Alt+K" }, { MENU_SESSIONS, EA_SESSION_MANAGE, "Manage Sessions...", "" },
    };
    struct editor_item list[EDITOR_ITEMS];
    for (size_t i = 0; i < sizeof where / sizeof where[0]; i++) {
        int n = editor_items(where[i].menu, list), found = 0;
        for (int k = 0; k < n; k++) {
            found |= list[k].action == where[i].action && strcmp(list[k].label, where[i].label) == 0 &&
                     strcmp(list[k].shortcut, where[i].keys) == 0;
        }
        if (!found) printf("  %s is not in menu %d with %s\n", where[i].label, where[i].menu, where[i].keys);
        CHECK(found);
    }
    for (int action = EA_NEW; action <= EA_PREV_TAB; action++) {
        int found = 0;
        for (int menu = 0; menu <= SUB_CODE_FOLDING && !found; menu++) {
            int n = editor_items(menu, list);
            for (int k = 0; k < n; k++) found |= list[k].action == action;
        }
        if (!found) printf("  action %d is in no menu\n", action);
        CHECK(found);
    }
    /* A submenu opens at the right of its item: View > Borders > Show Icon
     * Border; Go > Bookmarks, its list of bookmarks. */
    CHECK(!t->icons);
    for (int i = 0; i < 6; i++) push(K_DOWN, 0, NULL); /* Split View, Tool Views, 3 font sizes, Word Wrap, Borders */
    push('\n', 0, NULL);
    push('\n', 0, NULL);
    key('v', KEYS_ALT);
    CHECK(qhead == qtail && t->icons);
    int n = editor_items(SUB_BOOKMARKS, list);
    CHECK(n == 6 && strcmp(list[4].label, "1 - \"one\"") == 0 && strcmp(list[5].label, "4 - \"four\"") == 0);
    for (int i = 0; i < 12; i++) push(K_DOWN, 0, NULL); /* to Bookmarks */
    push('\n', 0, NULL);
    for (int i = 0; i < 5; i++) push(K_DOWN, 0, NULL); /* to "4 - "four"" */
    push('\n', 0, NULL);
    key('g', KEYS_ALT);
    CHECK(qhead == qtail && t->cursor == line_start(d, 3));
    puts("PASS menu grouping: find and replace under Edit, Go to Line, brackets, modified lines and Bookmarks under Go, "
         "View > Split View, Tool Views, Word Wrap, Borders, Code Folding submenus; every action reachable, shortcuts "
         "kept; a submenu cascades");

    reset();
    t = open_text("views.txt", "one\ntwo\n");
    key('t', KEYS_CTRL | KEYS_SHIFT); /* Split Horizontal */
    CHECK(nspaces == 2 && active == 1 && tab_of(0)->doc == tab_of(1)->doc);
    struct edit_rect upper = space_rect(0), lower = space_rect(1);
    CHECK(lower.y > upper.y && lower.x == upper.x && lower.w == upper.w);
    key('l', KEYS_CTRL | KEYS_SHIFT);
    CHECK(nspaces == 3 && active == 2);
    key(K_F1 + 7, 0); /* F8 */
    CHECK(active == 0);
    key(K_F1 + 7, 0);
    CHECK(active == 1);
    key(K_F1 + 7, KEYS_SHIFT); /* Shift+F8 */
    CHECK(active == 0);
    key(K_F1 + 7, KEYS_SHIFT);
    CHECK(active == 2);
    key('r', KEYS_CTRL | KEYS_SHIFT); /* Close Current View */
    CHECK(nspaces == 2 && ndocs == 1);
    key('r', KEYS_CTRL | KEYS_SHIFT);
    CHECK(nspaces == 1 && ndocs == 1 && cur_tab());
    key('r', KEYS_CTRL | KEYS_SHIFT);
    CHECK(nspaces == 1 && cur_tab()); /* the only view stays */
    puts("PASS views: Ctrl+Shift+T splits horizontally (the new view below, active), F8 and Shift+F8 go round, "
         "Ctrl+Shift+R closes the current view, never the last");

    /* Font zoom on the content font (a recording one here). */
    reset();
    t = open_text("zoom.md", "# Head\n\n*em* **strong** <https://example.org>\n");
    d = t->doc;
    syntax_parse(d->syntax, &d->buf, 0);
    font_stub = 1;
    key('=', KEYS_CTRL); /* Ctrl++ */
    CHECK(t->font_px == 20 && font_stub_px == 20);
    key('-', KEYS_CTRL);
    CHECK(t->font_px == 18 && font_stub_px == 18);
    pointer_at(10, 6, 0, 0, 0, 0, 1, KEYS_CTRL); /* Ctrl+wheel up */
    CHECK(t->font_px == 20);
    pointer_at(10, 6, 0, 0, 0, 0, -1, KEYS_CTRL);
    pointer_at(10, 6, 0, 0, 0, 0, -1, KEYS_CTRL);
    CHECK(t->font_px == 16 && font_stub_px == 16);
    int cw, ch, top;
    CHECK(content_metrics(0, &cw, &ch, &top) && cw == 9 && ch == 18);
    /* Bold, italic and underline reach the font with the run they style. */
    nfont_runs = 0;
    draw();
    unsigned head = 0, em = 0, strong = 0, url = 0;
    for (int i = 0; i < nfont_runs; i++) {
        if (strstr(font_runs[i].text, "Head")) head |= font_runs[i].style;
        if (strcmp(font_runs[i].text, "em") == 0) em |= font_runs[i].style;
        if (strstr(font_runs[i].text, "strong")) strong |= font_runs[i].style;
        if (strstr(font_runs[i].text, "example.org")) url |= font_runs[i].style;
    }
    CHECK((head & CONTENT_FONT_BOLD) && (em & CONTENT_FONT_ITALIC) && !(em & CONTENT_FONT_BOLD));
    CHECK((strong & CONTENT_FONT_BOLD) && (url & CONTENT_FONT_UNDERLINE));
    /* Ctrl+0 resets (keys.c gives it as K_CTRL_0). */
    key(K_CTRL_0, KEYS_CTRL);
    CHECK(t->font_px == 0);
    font_stub = 0;
    puts("PASS font zoom: Ctrl++ and Ctrl+- by two pixels, Ctrl+wheel, Ctrl+0 resets; the requested size reaches the "
         "content font; bold, italic and underline styles reach content_font_draw");
}

/* Kate's default tool view (the handbook's Quick Start; KateFileTreePlugin
 * makes its Documents tool view in the left sidebar): shown at the first
 * start; the session keeps the sidebar as the user left it. */
static void test_default_tool_view(void)
{
    CHECK(edit_defaults_sidebar == 1 && edit_defaults_side_view == SIDE_DOCUMENTS);
    reset();
    sidebar = edit_defaults_sidebar; /* as edit starts */
    side_view = edit_defaults_side_view;
    if (is_root) {
        make_dir("home");
        snprintf(kate_test_home, sizeof kate_test_home, "%s", at_dir("home"));
        snprintf(session_name, sizeof session_name, "default");
        session_enabled = session_blocked = session_last_error = 0;
        session_digest[0] = '\0';
        session_startup(); /* main's: no session yet */
        CHECK(!session_blocked && session_enabled && ndocs == 0);
    }
    CHECK(sidebar && side_view == SIDE_DOCUMENTS && !tree_focus);
    struct tab *t = open_text("first.txt", "first start\n");
    draw();
    int row = row_from(4, "first.txt");
    CHECK(row_has(2, "Documents") && grid[2][1].bg == C_BORDER && row_has(3, "New") && row >= 4 &&
          grid[row][col_of(row, "first.txt")].bg == C_FOCUS);
    int y0, h;
    text_rows(0, &y0, &h);
    CHECK(space_rect(0).x == SIDE_W + 1 && grid[y0][SIDE_W + 1 + gutter(t)].cp == 'f');
    if (is_root) {
        /* Hidden (Show Sidebars), quit: the next start restores it hidden. */
        key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
        CHECK(!sidebar && session_quit());
        reset();
        sidebar = edit_defaults_sidebar;
        side_view = edit_defaults_side_view;
        session_enabled = 0;
        session_digest[0] = '\0';
        session_startup();
        CHECK(!session_blocked && ndocs == 1 && !sidebar);
        /* Shown again from View > Tool Views: so at the next start. */
        editor_action(EA_DOCUMENTS);
        CHECK(sidebar && side_view == SIDE_DOCUMENTS && session_quit());
        reset();
        sidebar = 0; /* whatever it was: the session's applies */
        side_view = edit_defaults_side_view;
        session_enabled = 0;
        session_digest[0] = '\0';
        session_startup();
        CHECK(!session_blocked && ndocs == 1 && sidebar && side_view == SIDE_DOCUMENTS);
        session_enabled = 0;
        kate_test_home[0] = '\0';
    }
    reset();
    puts(is_root ? "PASS default tool view: the Documents list at the left of the editing area at the first start; "
                   "the session keeps it hidden or shown as left"
                 : "PASS default tool view: the Documents list at the left at the first start (session part: root only)");
}

/* View > Borders > Show Icon Border (F6), off by default (kateconfig.cpp
 * Icon Bar); a click on it sets or clears the line's bookmark. */
static void test_icon_border(void)
{
    reset();
    CHECK(edit_defaults_icons == 0 && view_icons_default == 0);
    struct tab *t = open_text("icons.txt", "zero\none\ntwo\nthree\nfour\n");
    struct doc *d = t->doc;
    CHECK(!t->icons && gutter(t) == 4);
    t->cursor = t->anchor = line_start(d, 1);
    key(K_F1 + 5, 0); /* F6 */
    CHECK(t->icons && gutter(t) == 4 + ICON_COLS);
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    /* The icon border, then the folding markers' column, the numbers, the
     * modification markers' column, the text. */
    CHECK(grid[y0][0].bg == C_BORDER && grid[y0][1].bg == C_BORDER && grid[y0][4].cp == '1' && grid[y0][6].cp == 'z');
    click(0, y0 + 2, BTN_LEFT, 1, 1);
    click(0, y0 + 2, BTN_LEFT, 0, 0);
    CHECK(bookmarked(d, 2) && t->cursor == line_start(d, 1) && !has_sel(t)); /* the cursor stays */
    draw();
    CHECK(grid[y0 + 2][0].bg == C_BOOKMARK && grid[y0 + 2][1].bg == C_BOOKMARK && grid[y0 + 1][0].bg == C_BORDER);
    click(1, y0 + 2, BTN_LEFT, 1, 1);
    click(1, y0 + 2, BTN_LEFT, 0, 0);
    CHECK(!bookmarked(d, 2));
    /* Released on another line than pressed: nothing. */
    click(0, y0 + 3, BTN_LEFT, 1, 1);
    click(0, y0 + 4, BTN_LEFT, 0, 0);
    CHECK(!bookmarked(d, 3) && !bookmarked(d, 4) && ui_of(d)->bookmarks.n == 0);
    /* The right button: the mark menu, its Bookmark chosen. */
    push('\n', 0, NULL);
    click(0, y0 + 4, BTN_RIGHT, 1, 1);
    click(0, y0 + 4, BTN_RIGHT, 0, 0);
    CHECK(qhead == qtail && bookmarked(d, 4));
    /* A bookmark set with Ctrl+B shows there too. */
    key('b', KEYS_CTRL);
    draw();
    CHECK(bookmarked(d, 1) && grid[y0 + 1][0].bg == C_BOOKMARK && grid[y0 + 4][1].bg == C_BOOKMARK);
    /* A click beside it, on the numbers, is the text's again. */
    click(4, y0 + 3, BTN_LEFT, 1, 1);
    click(4, y0 + 3, BTN_LEFT, 0, 0);
    CHECK(line_of(d, t->cursor) == 3 && !bookmarked(d, 3));
    key(K_F1 + 5, 0);
    CHECK(!t->icons && gutter(t) == 4);
    draw();
    CHECK(grid[y0 + 1][0].bg != C_BOOKMARK && grid[y0][4].cp == 'z');
    puts("PASS Show Icon Border (F6, off by default): at the left of the border, a left click on a line sets or "
         "clears its bookmark (the cursor stays), the right one the mark menu, the bookmark's mark drawn");
}

/* The line modification markers (Line Modification, on by default) in the
 * border, and Go to Previous/Next Modified Line. */
static void test_line_markers(void)
{
    reset();
    CHECK(LINE_MODIFICATION == 1);
    struct tab *t = open_text("marks.txt", "a0\na1\na2\na3\na4\na5\na6\na7\na8\na9\n");
    struct doc *d = t->doc;
    int y0, h, g = gutter(t);
    draw();
    text_rows(0, &y0, &h);
    for (int r = 0; r < 10; r++) CHECK(grid[y0 + r][g - 1].cp != 0x258C);
    t->cursor = t->anchor = line_end(d, 2);
    keys("x");
    t->cursor = t->anchor = line_end(d, 7);
    keys("y");
    draw();
    CHECK(grid[y0 + 2][g - 1].cp == 0x258C && grid[y0 + 2][g - 1].fg == C_MODIFIED && grid[y0 + 2][g - 1].bg == C_BORDER);
    CHECK(grid[y0 + 7][g - 1].fg == C_MODIFIED && grid[y0 + 3][g - 1].cp != 0x258C && grid[y0 + 2][g].cp == 'a');
    key('s', KEYS_CTRL); /* saved: SavedLines' colour */
    draw();
    CHECK(grid[y0 + 2][g - 1].cp == 0x258C && grid[y0 + 2][g - 1].fg == C_SAVED && grid[y0 + 7][g - 1].fg == C_SAVED);
    t->cursor = t->anchor = line_end(d, 5);
    keys("z");
    draw();
    CHECK(grid[y0 + 5][g - 1].fg == C_MODIFIED && grid[y0 + 2][g - 1].fg == C_SAVED);
    /* Without line numbers: a column of their own after the folding markers'. */
    key(K_F1 + 10, 0);
    draw();
    CHECK(gutter(t) == 2 && grid[y0 + 5][1].cp == 0x258C && grid[y0 + 5][1].fg == C_MODIFIED && grid[y0 + 5][2].cp == 'a');
    key(K_F1 + 10, 0);
    /* A wrapped line has it on each of its rows. */
    char long_line[400];
    memset(long_line, 'w', 300);
    long_line[300] = '\0';
    t->cursor = t->anchor = line_end(d, 0);
    type_text(active, t, long_line, 300);
    t->cursor = t->anchor = 0;
    show_cursor(0, t, 0);
    draw();
    CHECK(line_rows(t, text_width(0, t), 0) == 3 && grid[y0][g - 1].fg == C_MODIFIED && grid[y0 + 1][g - 1].cp == 0x258C &&
          grid[y0 + 2][g - 1].cp == 0x258C && grid[y0 + 3][g - 1].cp != 0x258C);
    /* A document without a file has none (Kate: only with a URL). */
    key('n', KEYS_CTRL);
    keys("new text");
    draw();
    CHECK(gutter(cur_tab()) == 4 && grid[y0][3].cp != 0x258C);
    key(K_F1 + 10, 0);
    CHECK(gutter(cur_tab()) == 1);
    show_doc(active, d);
    t = cur_tab();
    /* Go to Next and Previous Modified Line (no default shortcut, KTextEditor
     * has none): modified and saved lines, from the cursor's line, the
     * selection cleared, no cycling. */
    struct editor_item list[EDITOR_ITEMS];
    int n = editor_items(MENU_GO, list);
    for (int k = 0; k < n; k++) {
        if (list[k].action == EA_PREV_MODIFIED || list[k].action == EA_NEXT_MODIFIED) CHECK(!list[k].shortcut[0]);
    }
    t->cursor = line_start(d, 1);
    t->anchor = line_start(d, 1) + 1;
    editor_action(EA_NEXT_MODIFIED);
    CHECK(t->cursor == line_start(d, 2) && !has_sel(t));
    editor_action(EA_NEXT_MODIFIED);
    CHECK(t->cursor == line_start(d, 5));
    editor_action(EA_NEXT_MODIFIED);
    CHECK(t->cursor == line_start(d, 7));
    editor_action(EA_NEXT_MODIFIED);
    CHECK(t->cursor == line_start(d, 7)); /* the last: it stays */
    editor_action(EA_PREV_MODIFIED);
    CHECK(t->cursor == line_start(d, 5));
    /* From the Go menu (its 9th item). */
    for (int i = 0; i < 8; i++) push(K_DOWN, 0, NULL);
    push('\n', 0, NULL);
    key('g', KEYS_ALT);
    CHECK(qhead == qtail && t->cursor == line_start(d, 2));
    editor_action(EA_PREV_MODIFIED);
    CHECK(t->cursor == line_start(d, 0)); /* the long line typed into */
    editor_action(EA_PREV_MODIFIED);
    CHECK(t->cursor == line_start(d, 0));
    puts("PASS line modification markers (on by default): ModifiedLines and SavedLines colours right after the line "
         "numbers, on each row of a line, their own column without numbers, none without a file; Go to Previous/Next "
         "Modified Line from the Go menu, no default shortcut, no cycling");
}

/* A pointer motion over (col, row) at pixel row y. */
static void hover(int col, int row, int y)
{
    ptr_now = (struct pointer_event){ .x = col * 10 + 2, .y = y, .col = col, .row = row };
    mods_now = 0;
    key_event(K_POINTER);
}

/* Scroll Bar Preview (on by default): the lines at the hovered place. */
static void test_scroll_preview(void)
{
    reset();
    CHECK(SCROLL_PREVIEW == 1);
    view_scrollbar_default = edit_defaults_scrollbar;
    view_minimap_default = edit_defaults_minimap;
    char text[64 * 301] = "";
    for (int i = 0; i < 300; i++) {
        char line[64];
        snprintf(line, sizeof line, i % 2 ? "    int value%d;\n" : "\n", i);
        strcat(text, line);
    }
    struct tab *t = open_text("preview.c", text);
    struct doc *d = t->doc;
    syntax_parse(d->syntax, &d->buf, 0);
    draw();
    struct scroll_geom g;
    scroll_geometry(0, t, &g);
    CHECK(t->minimap && g.x == COLS - 6 && g.y == 3 && g.doc_rows == 26 && g.total == 301);
    /* Over the mini-map: nothing for 250 ms, then the lines at the
     * pointer's share of the document, in the middle of the box. */
    hover(g.x + 2, g.y + 13, (g.y + 13) * 20 + 5);
    draw();
    CHECK(row_from(3, "int value149;") < 0 && preview_timeout() > 0 && preview_timeout() <= 251);
    clock_now += 0.3;
    CHECK(preview_timeout() == 0);
    draw();
    /* 265 / 520 of 301 lines: line 153 in the middle of 8 (149 to 156); the
     * box half the view wide at the left of the mini-map. */
    int w = space_rect(0).w / 2, x = g.x - w;
    CHECK(w == 68 && row_from(3, "int value149;") == 12 && row_from(3, "int value155;") == 18);
    CHECK(col_of(12, "int value149;") == x + 1 + 4 && grid[12][x + 1 + 4].fg == 0x2980B9); /* its colours */
    CHECK(grid[11][x].bg == C_SEPARATOR && grid[12][x].bg == C_SEPARATOR && grid[12][x + 1].bg == C_BG &&
          grid[20][x + 5].bg == C_SEPARATOR);
    /* Shown: it follows the pointer at once. */
    hover(g.x + 2, g.y + 25, (g.y + 25) * 20 + 5);
    draw();
    CHECK(row_from(3, "int value149;") < 0 && row_from(3, "int value289;") == 21);
    /* At the end the last lines fill it. */
    hover(g.x + 2, g.y + 25, (g.y + 25) * 20 + 19);
    draw();
    CHECK(row_from(3, "int value293;") == 20 && row_from(3, "int value299;") == 26);
    /* Off the groove: gone. */
    hover(20, 10, 10 * 20 + 5);
    draw();
    CHECK(preview.space < 0 && row_from(3, "int value299;") < 0);
    /* A press takes it away. */
    hover(g.x + 2, g.y + 13, (g.y + 13) * 20 + 5);
    clock_now += 0.3;
    draw();
    CHECK(preview.shown && row_from(3, "int value149;") == 12);
    click(g.x + 2, g.y + 13, BTN_LEFT, 1, 1);
    click(g.x + 2, g.y + 13, BTN_LEFT, 0, 0);
    draw();
    CHECK(preview.space < 0 && grid[11][x].bg != C_SEPARATOR);
    /* Over the scrollbar without the mini-map. */
    editor_action(EA_MINIMAP);
    key(K_BOL, KEYS_CTRL);
    draw();
    scroll_geometry(0, t, &g);
    CHECK(!t->minimap && g.x == COLS - 1);
    hover(g.x, g.y + 13, (g.y + 13) * 20 + 5);
    clock_now += 0.3;
    draw();
    CHECK(row_from(3, "int value149;") == 12 && col_of(12, "int value149;") == g.x - w + 1 + 4);
    editor_action(EA_MINIMAP);
    /* A document that fits has no range to preview. */
    t = open_text("fits.c", "int a;\nint b;\n");
    draw();
    scroll_geometry(0, t, &g);
    hover(g.x + 2, g.y + 5, (g.y + 5) * 20 + 5);
    clock_now += 0.3;
    draw();
    for (int r = 3; r < ROWS - 1; r++) {
        for (int c = 0; c < g.x; c++) CHECK(grid[r][c].bg != C_SEPARATOR);
    }
    view_scrollbar_default = view_minimap_default = 0;
    reset();
    puts("PASS Scroll Bar Preview (on by default): 250 ms after the pointer comes over the mini-map or the scrollbar, "
         "the lines at its place in their colours, beside it; it follows the pointer, fills to the end, goes on a press "
         "or when the pointer leaves; none for a document that fits");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    setlocale(LC_CTYPE, "C.UTF-8");
    is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0 && realpath(argv[1], test_dir));
    if (is_root) CHECK(chown(test_dir, EDIT_USER, EDIT_USER) == 0);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    test_quick_open();
    test_documents();
    test_switcher();
    test_scrollbar();
    test_auto_brackets();
    test_remove_spaces();
    test_menus_views_zoom();
    test_icon_border();
    test_line_markers();
    test_scroll_preview();
    test_default_tool_view();
    reset();
    puts("ALL PASSED (test-edit-kate)");
    return 0;
}
