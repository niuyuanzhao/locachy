/* Include the real editor and its screen/key/pointer harness, unmodified.
 * This file exercises actual draw_text/move_rows/hit testing/search paths;
 * a helper-only model cannot pass when edit.c only draws folding markers. */
#ifndef EDIT_FOLDING_UI_HARNESS
#error "Build with test-edit-folding-ui.sh (renames only the existing harness main)"
#endif
#include "base.h"
#include "i18n-stub.h"
#include EDIT_FOLDING_UI_HARNESS

static const char folding_code[] =
    "void outer() {\n"
    "  if (enabled) {\n"
    "    while (ready) {\n"
    "      hidden_needle();\n"
    "    }\n"
    "    inner_tail();\n"
    "  }\n"
    "  outer_tail();\n"
    "}\n"
    "void after() {}\n";

static void folding_reset(void)
{
    for (int s = 0; s < nspaces; s++)
        for (int i = 0; i < spaces[s].ntabs; i++) fold_release(&spaces[s].tabs[i]);
    reset();
}

static struct tab *folding_open(void)
{
    folding_reset();
    struct tab *t = open_text("folding.c", folding_code);
    CHECK(t->doc->syntax && syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '-' && fold_marker(t, 2) == '-');
    return t;
}

static void folding_position(struct tab *t, size_t line)
{
    t->cursor = t->anchor = buffer_line_start(&t->doc->buf, line);
    t->goal = -1;
}

static void test_folding_draw_and_navigation(void)
{
    struct tab *t = folding_open();
    folding_position(t, 3);
    t->top = buffer_line_start(&t->doc->buf, 3);
    CHECK(fold_current(t, 1, 0));
    CHECK(line_of(t->doc, t->cursor) == 2 && line_of(t->doc, t->top) == 2);
    CHECK(fold_marker(t, 2) == '+' && fold_next(t, 2) == 5 && fold_prev(t, 5) == 2);
    t->top = 0;
    draw();
    CHECK(on_screen("while (ready)") >= 0 && on_screen("hidden_needle") < 0);
    CHECK(on_screen("inner_tail") >= 0 && on_screen("outer_tail") >= 0 && on_screen("void after") >= 0);
    /* Actual editor key path must skip hidden body AND closing brace. */
    key(K_DOWN, 0);
    CHECK(line_of(t->doc, t->cursor) == 5 && fold_marker(t, 2) == '+');
    key(K_UP, 0);
    CHECK(line_of(t->doc, t->cursor) == 2 && fold_marker(t, 2) == '+');
    /* Kate's WrappingCursor moves Left/Right by DOCUMENT lines and its
     * updateCursor calls ensureLineIsVisible: crossing into the body opens. */
    t->cursor = t->anchor = line_end(t->doc, 2);
    key(K_RIGHT, 0);
    CHECK(line_of(t->doc, t->cursor) == 3 && t->cursor == buffer_line_start(&t->doc->buf, 3));
    CHECK(fold_marker(t, 2) == '-');
    CHECK(fold_toggle(t, 2, 0) && line_of(t->doc, t->cursor) == 2);
    t->cursor = t->anchor = buffer_line_start(&t->doc->buf, 5);
    key(K_LEFT, 0);
    CHECK(line_of(t->doc, t->cursor) == 4 && t->cursor == line_end(t->doc, 4) && fold_marker(t, 2) == '-');
    CHECK(fold_toggle(t, 2, 0));
    folding_position(t, 2);
    /* Actual move_rows is shared by PageUp/Down, wheel and point_at. */
    size_t line = 2;
    int row = 0;
    move_rows(t, text_width(active, t), &line, &row, 1);
    CHECK(line == 5 && row == 0);
    move_rows(t, text_width(active, t), &line, &row, -1);
    CHECK(line == 2 && row == 0);
    int y, h, x, w;
    text_rows(active, &y, &h);
    space_cols(active, &x, &w);
    struct pointer_event p = { .col = x + gutter(t) + 1, .row = y + 3,
                               .x = (x + gutter(t) + 1) * 10, .y = (y + 3) * 20 };
    size_t at;
    CHECK(point_at(active, &p, &at, 0));
    CHECK(line_of(t->doc, at) == 5);
    /* A targeted jump/search must reveal, not normalize the target away. */
    move_to(active, t, buffer_line_start(&t->doc->buf, 3), 0, 0);
    CHECK(line_of(t->doc, t->cursor) == 3 && fold_marker(t, 2) == '-');
    draw();
    CHECK(on_screen("hidden_needle") >= 0);
    puts("PASS folding real editor: hidden drawing, vertical/horizontal keys, visual rows, pointer hit-testing and jump reveal");
}

static void test_folding_hierarchy(void)
{
    struct tab *t = folding_open();
    CHECK(fold_toggle(t, 2, 0));
    CHECK(fold_toggle(t, 1, 0));
    CHECK(fold_top_level(t, 1, 0));
    CHECK(fold_marker(t, 0) == '+' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    CHECK(fold_line(t, 3) == 0 && fold_next(t, 0) == 9);
    CHECK(fold_top_level(t, 0, 0));
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    CHECK(fold_line(t, 3) == 1);
    CHECK(fold_toggle(t, 1, 0));
    CHECK(fold_line(t, 3) == 2 && fold_marker(t, 2) == '+');
    CHECK(fold_toggle(t, 0, 1));
    CHECK(fold_marker(t, 0) == '+' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    CHECK(fold_toggle(t, 0, 1));
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '-' && fold_marker(t, 2) == '-');
    CHECK(fold_top_level(t, 1, 1));
    CHECK(fold_reveal(t, 1));
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    CHECK(fold_reveal(t, 3));
    CHECK(fold_marker(t, 1) == '-' && fold_marker(t, 2) == '-');
    puts("PASS folding real editor: current/top-level/recursive semantics and normal unfold preserves descendants");
}

static void test_folding_mapping_split_and_edit(void)
{
    struct tab *t = folding_open();
    CHECK(fold_toggle(t, 2, 0));
    size_t visible[] = { 0, 1, 2, 5, 6, 7, 8, 9, 10 };
    for (size_t i = 0; i < sizeof visible / sizeof visible[0]; i++) {
        CHECK(fold_document_line(t, i) == visible[i]);
        CHECK(fold_visible_index(t, visible[i]) == i);
    }
    CHECK(fold_visible_index(t, 3) == 2 && fold_visible_index(t, 4) == 2);
    /* A separate actual editor view of the same doc has independent state. */
    nspaces = 2;
    spaces[1].cur = -1;
    struct tab *other = show_doc(1, t->doc);
    CHECK(other && fold_marker(other, 2) == '-' && fold_next(other, 2) == 3);
    CHECK(fold_marker(t, 2) == '+');
    /* The actual document moved callback adjusts owned range snapshots so
     * an insertion before a fold preserves the matching new grammar node. */
    CHECK(doc_insert(t->doc, 0, "// prefix\n", 10) == 0);
    CHECK(fold_next(t, 3) == 4); /* dirty tree exposes all lines */
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(fold_marker(t, 3) == '+' && fold_next(t, 3) == 6);
    CHECK(fold_toggle(t, 3, 0));
    CHECK(fold_marker(t, 3) == '-');
    /* Delete the inner region, then no old hidden interval may survive. */
    size_t start = buffer_line_start(&t->doc->buf, 3), end = buffer_line_start(&t->doc->buf, 6);
    CHECK(fold_toggle(t, 3, 0));
    doc_remove(t->doc, start, end - start);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(fold_marker(t, 3) == 0 && fold_next(t, 3) == 4);
    fold_release(other);
    puts("PASS folding real editor: visible-line bijection, independent split views and actual incremental document edits");
}

static void test_folding_gutter_and_actions(void)
{
    struct tab *t = folding_open();
    draw();
    int x, w, y, h;
    space_cols(active, &x, &w);
    text_rows(active, &y, &h);
    click(x + gutter(t) - 1, y + 2, BTN_LEFT, 1, 1);
    click(x + gutter(t) - 1, y + 2, BTN_LEFT, 0, 1);
    CHECK(fold_marker(t, 2) == '+');
    draw();
    CHECK(on_screen("hidden_needle") < 0);
    click(x + gutter(t) - 1, y + 2, BTN_LEFT, 1, 1);
    click(x + gutter(t) - 1, y + 2, BTN_LEFT, 0, 1);
    CHECK(fold_marker(t, 2) == '-');
    folding_position(t, 3);
    editor_action(EA_FOLD_CURRENT);
    CHECK(fold_marker(t, 2) == '+' && line_of(t->doc, t->cursor) == 2);
    editor_action(EA_UNFOLD_CURRENT);
    CHECK(fold_marker(t, 2) == '-');
    editor_action(EA_FOLD_ALL);
    CHECK(fold_marker(t, 0) == '+' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    editor_action(EA_UNFOLD_TOP);
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    editor_action(EA_UNFOLD_ALL);
    CHECK(fold_marker(t, 1) == '-' && fold_marker(t, 2) == '-');
    key('-', KEYS_CTRL | KEYS_SHIFT);
    CHECK(fold_marker(t, 0) == '+');
    key('+', KEYS_CTRL | KEYS_SHIFT);
    CHECK(fold_marker(t, 0) == '-');
    puts("PASS folding real editor: actual gutter click, menu commands and top-level keyboard shortcuts");
}

static void folding_click(int col, int row, int mods)
{
    for (int pressed = 1; pressed >= 0; pressed--) {
        ptr_now = (struct pointer_event){ .x = col * 10 + 2, .y = row * 20 + 5, .col = col, .row = row,
                                          .button = BTN_LEFT, .pressed = pressed, .clicks = 1,
                                          .buttons = pressed };
        mods_now = mods;
        key_event(K_POINTER);
        clock_now += 0.01;
    }
    mods_now = 0;
}

/* Shift+click on the marker is recursive; folding is view state only: the
 * text, undo history and saved file keep every hidden byte. */
static void test_folding_recursive_click_data_undo(void)
{
    struct tab *t = folding_open();
    draw();
    int x, w, y, h;
    space_cols(active, &x, &w);
    text_rows(active, &y, &h);
    folding_click(x + gutter(t) - 1, y, KEYS_SHIFT);
    CHECK(fold_marker(t, 0) == '+' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    draw();
    CHECK(on_screen("void outer") >= 0 && on_screen("if (enabled)") < 0 && on_screen("void after") >= 0);
    folding_click(x + gutter(t) - 1, y, 0); /* plain: only the outer one opens */
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '+' && fold_marker(t, 2) == '+');
    folding_click(x + gutter(t) - 1, y, KEYS_SHIFT);
    folding_click(x + gutter(t) - 1, y, KEYS_SHIFT);
    CHECK(fold_marker(t, 0) == '-' && fold_marker(t, 1) == '-' && fold_marker(t, 2) == '-');

    CHECK(fold_toggle(t, 2, 0));
    char *before = text_of(t->doc);
    CHECK(before && strcmp(before, folding_code) == 0 && !doc_modified(t->doc));
    free(before);
    folding_position(t, 0);
    keys("x");
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(fold_marker(t, 2) == '+' && fold_next(t, 2) == 5);
    key('s', KEYS_CTRL);
    CHECK(strstr(file_bytes(at_dir("folding.c")), "hidden_needle();") != NULL);
    key('z', KEYS_CTRL);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    char *after = text_of(t->doc);
    CHECK(after && strcmp(after, folding_code) == 0);
    free(after);
    CHECK(fold_marker(t, 2) == '+' && fold_next(t, 2) == 5);
    key('z', KEYS_CTRL | KEYS_SHIFT);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(byte_at(t->doc, 0) == 'x' && fold_marker(t, 2) == '+');
    /* Ctrl+Right (Kate's wordNext, a WrappingCursor too) from the header's
     * end lands in the body, which opens. */
    t->cursor = t->anchor = line_end(t->doc, 2);
    key(K_RIGHT, KEYS_CTRL);
    CHECK(line_of(t->doc, t->cursor) == 3 && fold_marker(t, 2) == '-');
    puts("PASS folding real editor: Shift+click recursive marker; text/save/undo/redo keep hidden bytes and fold state; Ctrl+Right opens as Kate");
}

/* A fold reaching the end of a file without a final line break: the view
 * cannot be scrolled into it; Down on the last visible line goes to that
 * line's end (Kate's cursorDown: moveCursorToLineEnd) and keeps it closed;
 * Right and Ctrl+End (document positions) open it. */
static void test_folding_end_of_file(void)
{
    folding_reset();
    const char *text = "first();\nvoid last() {\n  hidden_last();\n}";
    struct tab *t = open_text("folding-eof.c", text);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(t->doc->buf.lines == 4 && fold_marker(t, 1) == '-');
    CHECK(fold_toggle(t, 1, 0));
    CHECK(fold_next(t, 1) == t->doc->buf.lines && fold_line(t, 3) == 1);
    scroll_rows(active, t, 1000);
    CHECK(fold_line(t, line_of(t->doc, t->top)) == line_of(t->doc, t->top) && line_of(t->doc, t->top) <= 1);
    draw();
    CHECK(on_screen("void last") >= 0 && on_screen("hidden_last") < 0);
    folding_position(t, 1);
    key(K_DOWN, 0);
    CHECK(t->cursor == line_end(t->doc, 1) && fold_marker(t, 1) == '+');
    key(K_RIGHT, 0);
    CHECK(line_of(t->doc, t->cursor) == 2 && fold_marker(t, 1) == '-');
    CHECK(fold_toggle(t, 1, 0) && line_of(t->doc, t->cursor) == 1);
    key(K_EOL, KEYS_CTRL);
    CHECK(t->cursor == doc_len(t->doc) && fold_marker(t, 1) == '-');
    draw();
    CHECK(on_screen("hidden_last") >= 0);
    puts("PASS folding real editor: fold to EOF clamps scrolling; Down stays on the header's end, Right and Ctrl+End open it");
}

static void test_folding_page_scroll(void)
{
    folding_reset();
    char text[8192];
    size_t used = (size_t)snprintf(text, sizeof text, "void huge() {\n");
    for (int i = 0; i < 80; i++)
        used += (size_t)snprintf(text + used, sizeof text - used, "  hidden_page_%d();\n", i);
    used += (size_t)snprintf(text + used, sizeof text - used, "}\n");
    for (int i = 0; i < 70; i++)
        used += (size_t)snprintf(text + used, sizeof text - used, "visible_page_%d();\n", i);
    struct tab *t = open_text("folding-pages.c", text);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    CHECK(fold_toggle(t, 0, 0));
    int y, h;
    text_rows(active, &y, &h);
    key(K_PGDN, 0);
    CHECK(line_of(t->doc, t->cursor) == 81 + (size_t)(h - 1));
    CHECK(fold_marker(t, 0) == '+');
    key(K_PGUP, 0);
    CHECK(line_of(t->doc, t->cursor) == 0 && t->top == 0);
    scroll_rows(active, t, 1);
    CHECK(line_of(t->doc, t->top) == 82);
    draw();
    CHECK(on_screen("hidden_page_") < 0 && on_screen("visible_page_0") >= 0);
    scroll_rows(active, t, -1);
    CHECK(t->top == 0);
    puts("PASS folding real editor: PageUp/PageDown and wheel scroll use visible rows, not hidden document lines");
}

static void test_folding_search_and_wrap(void)
{
    struct tab *t = folding_open();
    CHECK(fold_toggle(t, 0, 0));
    draw();
    CHECK(on_screen("hidden_needle") < 0 && on_screen("void outer") >= 0 && on_screen("void after") >= 0);
    key('f', KEYS_CTRL);
    keys("hidden_needle");
    CHECK(bar.match == MATCH_FOUND && line_of(t->doc, t->cursor) == 3);
    key(27, 0);
    draw();
    CHECK(fold_marker(t, 0) == '-' && on_screen("hidden_needle") >= 0);
    /* A wrapped HEADER remains visible in full, and the next visual-row
     * movement skips only its folded document body. */
    char code[512];
    snprintf(code, sizeof code, "void %s() {\n  hidden_wrapped();\n}\nvisible_after();\n", "a_very_long_function_name_that_wraps_in_a_narrow_editor_view");
    CHECK(doc_set_text(t->doc, code, strlen(code)) == 0);
    CHECK(syntax_parse(t->doc->syntax, &t->doc->buf, 0) == 1);
    folding_position(t, 0);
    CHECK(fold_toggle(t, 0, 0));
    int rows = line_rows(t, 18, 0);
    CHECK(rows > 1);
    size_t line = 0;
    int row = 0;
    move_rows(t, 18, &line, &row, rows - 1);
    CHECK(line == 0 && row == rows - 1);
    move_rows(t, 18, &line, &row, 1);
    CHECK(line == 3 && row == 0);
    puts("PASS folding real editor: actual Find reveals folded matches and wrapped header traversal preserves every header row");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    setlocale(LC_CTYPE, "C.UTF-8");
    is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0);
    char *real = realpath(argv[1], NULL);
    CHECK(real);
    snprintf(test_dir, sizeof test_dir, "%s", real);
    free(real);
    if (is_root) CHECK(chown(test_dir, EDIT_USER, EDIT_USER) == 0);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    const char *which = getenv("EDIT_FOLDING_CASE");
    if (!which || strcmp(which, "navigation") == 0) test_folding_draw_and_navigation();
    if (!which || strcmp(which, "hierarchy") == 0) test_folding_hierarchy();
    if (!which || strcmp(which, "mapping") == 0) test_folding_mapping_split_and_edit();
    if (!which || strcmp(which, "gutter-actions") == 0) test_folding_gutter_and_actions();
    if (!which || strcmp(which, "recursive-data-undo") == 0) test_folding_recursive_click_data_undo();
    if (!which || strcmp(which, "eof") == 0) test_folding_end_of_file();
    if (!which || strcmp(which, "page") == 0) test_folding_page_scroll();
    if (!which || strcmp(which, "search-wrap") == 0) test_folding_search_and_wrap();
    folding_reset();
    puts("PASS actual editor folding UI tests");
    return 0;
}
