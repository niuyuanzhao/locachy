/* Reuse only the in-memory device/clipboard fixture, not a second editor.
 * Renaming its test main leaves edit.c, its real key/pointer/IME paths and
 * multicursor include in this translation unit. No input devices are opened. */
#ifndef EDIT_MULTICURSOR_FIXTURE
#error Build with tools/test-edit-multicursor.sh (generates a renamed device fixture)
#endif
#include "base.h"
#include "i18n-stub.h"
#include EDIT_MULTICURSOR_FIXTURE

static struct tab *mc_fixture(const char *text)
{
    for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) mc_free(&spaces[s].tabs[i]);
    reset();
    new_document();
    struct tab *t = cur_tab();
    CHECK(t && doc_set_text(t->doc, text, strlen(text)) == 0);
    t->cursor = t->anchor = 0;
    return t;
}
static void mc_key(int k, int mods) { key(k, mods); }
static void mc_pointer(int col, int row, int mods, int button, int pressed, int buttons)
{
    ptr_now = (struct pointer_event){ .x = col * 10, .y = row * 20 + 5, .col = col, .row = row,
        .button = button, .pressed = pressed, .clicks = 1, .buttons = buttons };
    mods_now = mods; key_event(K_POINTER);
}
static void mc_check_caret(struct tab *t, size_t cursor, size_t anchor)
{
    struct edit_multicursor *m = mc_state(t);
    int found = 0;
    for (size_t i = 0; i < m->n; i++) if (m->carets[i].cursor == cursor && m->carets[i].anchor == anchor) found = 1;
    if (!found) {
        printf("  missing cursor=%zu anchor=%zu, %zu carets:", cursor, anchor, m->n);
        for (size_t i = 0; i < m->n; i++) printf(" %zu/%zu", m->carets[i].cursor, m->carets[i].anchor);
        puts("");
    }
    CHECK(found);
}
static void mc_creation_test(void)
{
    struct tab *t = mc_fixture("abcd\nabcd\nabcd\n");
    mc_key(K_RIGHT, 0);
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    CHECK(mc_count(t) == 3);
    mc_check_caret(t, 1, 1); mc_check_caret(t, 6, 6); mc_check_caret(t, 11, 11);
    mc_fixture_caret_paints = 0; draw(); CHECK(mc_fixture_caret_paints == 3);
    int y, h; text_rows(0, &y, &h);
    mc_pointer(gutter(t) + 1, y + 1, KEYS_ALT, BTN_LEFT, 1, 1);
    mc_pointer(gutter(t) + 1, y + 1, KEYS_ALT, BTN_LEFT, 0, 0);
    CHECK(mc_count(t) == 2);
    mc_pointer(gutter(t) + 2, y + 1, KEYS_ALT, BTN_LEFT, 1, 1);
    mc_pointer(gutter(t) + 2, y + 1, KEYS_ALT, BTN_LEFT, 0, 0);
    CHECK(mc_count(t) == 3);
    mc_check_caret(t, 7, 7);
    mc_key(27, 0); CHECK(mc_count(t) == 1);
    t->anchor = 1; t->cursor = 10;
    mc_key('i', KEYS_ALT | KEYS_SHIFT);
    CHECK(mc_count(t) == 3); mc_check_caret(t, 4, 4); mc_check_caret(t, 9, 9); mc_check_caret(t, 14, 14);
    t = mc_fixture("abcd\nabcd\nabcd\n");
    mc_key(K_RIGHT, 0); mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key(K_RIGHT, 0); mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_check_caret(t, 12, 12); /* column must follow horizontal movement */
    t = mc_fixture("abc\nabc\n"); draw(); text_rows(0, &y, &h);
    mc_pointer(gutter(t) + 1, y + 1, KEYS_ALT, BTN_LEFT, 1, 1);
    mc_pointer(gutter(t) + 3, y + 1, KEYS_ALT, 0, 0, 1);
    mc_pointer(gutter(t) + 3, y + 1, KEYS_ALT, BTN_LEFT, 0, 0);
    CHECK(mc_count(t) == 2); mc_check_caret(t, 7, 5);
    keys("Q"); CHECK(text_is(t->doc, "Qabc\naQ\n"));
    puts("PASS creation: Ctrl+Alt+arrows, Alt+mouse toggle/selection-drag, selection line ends and Escape through real input paths");
}
static void mc_edit_test(void)
{
    struct tab *t = mc_fixture("abc\nabc\n"); struct doc *d = t->doc;
    mc_key(K_RIGHT, 0); mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    keys("X"); CHECK(text_is(d, "aXbc\naXbc\n")); CHECK(d->done == 1);
    mc_check_caret(t, 2, 2); mc_check_caret(t, 7, 7);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    CHECK(mc_count(t) == 2); mc_check_caret(t, 1, 1); mc_check_caret(t, 5, 5);
    mc_key('z', KEYS_CTRL | KEYS_SHIFT); CHECK(text_is(d, "aXbc\naXbc\n"));
    CHECK(mc_count(t) == 2); mc_check_caret(t, 2, 2); mc_check_caret(t, 7, 7);
    mc_key('\b', 0); CHECK(text_is(d, "abc\nabc\n"));
    mc_key(K_DEL, 0); CHECK(text_is(d, "ac\nac\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    mc_key(K_RIGHT, KEYS_SHIFT); mc_key('c', KEYS_CTRL);
    CHECK(strcmp(clip[0], "b\nb") == 0);
    draw(); int text_y, text_h; text_rows(0, &text_y, &text_h);
    CHECK(grid[text_y][gutter(t) + 1].bg == C_SELECT);
    CHECK(grid[text_y + 1][gutter(t) + 1].bg == C_SELECT);
    keys("Q"); CHECK(text_is(d, "aQc\naQc\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    mc_check_caret(t, 2, 1); mc_check_caret(t, 6, 5);
    char paste_data[] = "P\r\nQ";
    paste_text(0, t, paste_data, strlen(paste_data));
    CHECK(text_is(d, "aPc\naQc\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    mc_key('x', KEYS_CTRL); CHECK(text_is(d, "ac\nac\n"));
    CHECK(strcmp(clip[0], "b\nb") == 0);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    mc_deselect(t); char fallback[] = "x\ny\nz";
    paste_text(0, t, fallback, strlen(fallback)); CHECK(text_is(d, "abx\ny\nzc\nabx\ny\nzc\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    puts("PASS edit: typing, selection replacement, UTF-8 clipboard normalization, backspace/delete/cut and one-step undo/redo restore every caret/anchor");
}
static void mc_newline_indent_test(void)
{
    struct tab *t = mc_fixture("  one\n  two\n"); struct doc *d = t->doc;
    t->cursor = t->anchor = 5;
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key('\n', 0); CHECK(text_is(d, "  one\n  \n  two\n  \n"));
    CHECK(d->done == 1);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "  one\n  two\n"));
    mc_key('i', KEYS_CTRL); CHECK(text_is(d, "    one\n    two\n"));
    mc_key('i', KEYS_CTRL | KEYS_SHIFT); CHECK(text_is(d, "one\ntwo\n"));
    mc_key(K_BOL, KEYS_CTRL); /* merging coincident moved carets */
    CHECK(mc_count(t) == 1);
    t = mc_fixture("foo bar\nfoo bar\n"); d = t->doc;
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key(K_DEL, KEYS_CTRL); CHECK(text_is(d, "bar\nbar\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "foo bar\nfoo bar\n"));
    mc_key(K_RIGHT, KEYS_CTRL); mc_key('\b', KEYS_CTRL); CHECK(text_is(d, "bar\nbar\n"));
    puts("PASS newline/indent/word deletion: original Kate smart-indent and word semantics at every caret, one transaction");
}
static void mc_unicode_test(void)
{
    struct tab *t = mc_fixture("a中e\xcc\x81z\na中e\xcc\x81z\n"); struct doc *d = t->doc;
    mc_key(K_RIGHT, 0); mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key(K_RIGHT, 0); mc_check_caret(t, 4, 4); mc_check_caret(t, 13, 13);
    mc_key(K_DEL, 0); CHECK(text_is(d, "a中z\na中z\n"));
    mc_key('z', KEYS_CTRL); mc_key(K_RIGHT, KEYS_SHIFT);
    mc_check_caret(t, 7, 4); mc_check_caret(t, 16, 13);
    type_text(0, t, "好", strlen("好")); CHECK(text_is(d, "a中好z\na中好z\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "a中e\xcc\x81z\na中e\xcc\x81z\n"));
    t = mc_fixture("  a\n  b\n"); d = t->doc;
    t->cursor = t->anchor = 3; mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key('\n', KEYS_CTRL); CHECK(text_is(d, "  a\n\n  b\n\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "  a\n  b\n"));
    mc_key('\n', KEYS_SHIFT); CHECK(text_is(d, "  a\n  \n  b\n  \n"));
    t = mc_fixture("(a)\n(b)\n"); d = t->doc;
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    to_bracket(0, t, 1); mc_check_caret(t, 3, 0); mc_check_caret(t, 7, 4);
    CHECK(mc_count(t) == 2);
    puts("PASS Unicode: wide glyphs, combining-cluster movement/selection/Delete, matching-bracket selections and IME-style UTF-8 commit at every caret");
}
static void mc_occurrence_test(void)
{
    struct tab *t = mc_fixture("cat dog cat catfish cat\n"); struct doc *d = t->doc;
    mc_key('j', KEYS_ALT); CHECK(mc_count(t) == 1); CHECK(t->anchor == 0 && t->cursor == 3);
    mc_key('j', KEYS_ALT); CHECK(mc_count(t) == 2); mc_check_caret(t, 11, 8);
    mc_key('k', KEYS_ALT); mc_key('j', KEYS_ALT);
    CHECK(mc_count(t) == 2); mc_check_caret(t, 3, 0); mc_check_caret(t, 15, 12);
    mc_key('j', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT); CHECK(mc_count(t) == 4);
    keys("fox"); CHECK(text_is(d, "fox dog fox foxfish fox\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "fo dog fo fofish fo\n")); /* each key one transaction */
    mc_key('z', KEYS_CTRL); mc_key('z', KEYS_CTRL);
    CHECK(text_is(d, "cat dog cat catfish cat\n"));
    mc_key('j', KEYS_ALT); CHECK(mc_count(t) == 4);
    puts("PASS occurrences: Alt+J initial/next, Alt+K skip, select-all, Kate substring search defaults, cycling and selected replacement");
}
static void mc_block_test(void)
{
    struct tab *t = mc_fixture("abcd\nABCD\nx\n"); struct doc *d = t->doc;
    mc_key(K_RIGHT, 0); mc_key('b', KEYS_CTRL | KEYS_SHIFT);
    mc_key(K_DOWN, KEYS_SHIFT); mc_key(K_DOWN, KEYS_SHIFT);
    mc_key(K_RIGHT, KEYS_SHIFT); mc_key(K_RIGHT, KEYS_SHIFT);
    CHECK(mc_block(t) && mc_count(t) == 3);
    mc_key('c', KEYS_CTRL); CHECK(strcmp(clip[0], "bc\nBC\n  ") == 0);
    draw(); CHECK(on_screen("[BLOCK]") >= 0);
    keys("Q"); CHECK(text_is(d, "aQd\nAQD\nxQ\n"));
    CHECK(d->done == 1);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abcd\nABCD\nx\n"));
    CHECK(mc_block(t) && mc_count(t) == 3 && t->mc->rectangle);
    char paste_rows[] = "11\n22\n33";
    paste_text(0, t, paste_rows, strlen(paste_rows));
    CHECK(text_is(d, "a11d\nA22D\nx33\n"));
    mc_key('z', KEYS_CTRL); mc_key('x', KEYS_CTRL); CHECK(text_is(d, "ad\nAD\nx\n"));
    mc_key('z', KEYS_CTRL); mc_key('b', KEYS_CTRL | KEYS_SHIFT); CHECK(!mc_block(t));
    puts("PASS rectangle: Ctrl+Shift+B and Shift-arrows, selected columns/copy, short-line spaces, row-distributed paste, cut and restored block undo");
}
static void mc_block_mouse_tabs_test(void)
{
    struct tab *t = mc_fixture("\tleft\n\tright\nx\n"); struct doc *d = t->doc;
    mc_key('b', KEYS_CTRL | KEYS_SHIFT); draw();
    int y, h; text_rows(0, &y, &h);
    mc_pointer(gutter(t) + 1, y, 0, BTN_LEFT, 1, 1);
    mc_pointer(gutter(t) + 3, y + 2, 0, 0, 0, 1);
    mc_pointer(gutter(t) + 3, y + 2, 0, BTN_LEFT, 0, 0);
    CHECK(mc_count(t) == 3);
    draw();
    CHECK(grid[y][gutter(t)].bg != C_SELECT && grid[y][gutter(t) + 1].bg == C_SELECT &&
          grid[y][gutter(t) + 2].bg == C_SELECT && grid[y][gutter(t) + 3].bg != C_SELECT);
    CHECK(grid[y + 2][gutter(t) + 1].bg == C_SELECT && grid[y + 2][gutter(t) + 2].bg == C_SELECT);
    mc_key('c', KEYS_CTRL); CHECK(strcmp(clip[0], "  \n  \n  ") == 0);
    mc_key(K_DEL, 0); CHECK(text_is(d, "  left\n  right\nx\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "\tleft\n\tright\nx\n"));
    keys("Q"); CHECK(text_is(d, " Q left\n Q right\nxQ\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "\tleft\n\tright\nx\n"));
    t = mc_fixture("a中b\na中b\n"); d = t->doc;
    t->cursor = t->anchor = 1;
    mc_key('b', KEYS_CTRL | KEYS_SHIFT);
    mc_key(K_DOWN, KEYS_SHIFT); mc_key(K_RIGHT, KEYS_SHIFT); mc_key(K_RIGHT, KEYS_SHIFT);
    mc_key('c', KEYS_CTRL); CHECK(strcmp(clip[0], "中\n中") == 0);
    keys("Q"); CHECK(text_is(d, "aQb\naQb\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "a中b\na中b\n"));
    puts("PASS rectangular mouse: columns through partial tabs and wide UTF-8, virtual short lines, preservation outside edges and exact restoration on undo");
}
static void mc_overlap_test(void)
{
    struct tab *t = mc_fixture("abcdef\n"); struct doc *d = t->doc;
    t->anchor = 0; t->cursor = 3;
    mc_add(t, (struct mc_caret){ 5, 2, -1, -1, -1 }, 0);
    keys("Q"); CHECK(text_is(d, "Qf\n") && mc_count(t) == 1 && d->done == 1);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abcdef\n") && mc_count(t) == 2);
    mc_check_caret(t, 3, 0); mc_check_caret(t, 5, 2);
    t = mc_fixture("abc def\n"); d = t->doc;
    t->cursor = t->anchor = 1;
    mc_add(t, (struct mc_caret){ 2, 2, -1, -1, -1 }, 0);
    mc_key(K_DEL, KEYS_CTRL); CHECK(text_is(d, "adef\n") && mc_count(t) == 1);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc def\n") && mc_count(t) == 2);
    mc_check_caret(t, 1, 1); mc_check_caret(t, 2, 2);
    t = mc_fixture("  abc\n  def\n"); d = t->doc;
    t->cursor = t->anchor = 1;
    mc_add(t, (struct mc_caret){ 2, 2, -1, -1, -1 }, 0);
    mc_key('\t', 0); CHECK(text_is(d, "    abc\n  def\n"));
    mc_key('i', KEYS_CTRL); CHECK(text_is(d, "        abc\n  def\n"));
    mc_key('k', KEYS_CTRL); CHECK(text_is(d, "  def\n"));
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "        abc\n  def\n"));
    t->doc->readonly = 1; keys("X"); CHECK(text_is(d, "        abc\n  def\n"));
    t->doc->readonly = 0;
    t = mc_fixture("a\nb\n"); d = t->doc;
    d->lang = syntax_detect("fixture.c", &d->buf);
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT);
    mc_key('d', KEYS_CTRL); CHECK(text_is(d, "// a\n// b\n"));
    CHECK(d->done == 1);
    mc_key('d', KEYS_CTRL | KEYS_SHIFT); CHECK(text_is(d, "a\nb\n"));
    t = mc_fixture("aa\n\nbb\n");
    t->cursor = t->anchor = 2;
    mc_add_vertical(0, t, 1); mc_add_vertical(0, t, 1);
    CHECK(mc_count(t) == 3); mc_remove_empty(t); CHECK(mc_count(t) == 2);
    CHECK(doc_set_text(t->doc, "reload\n", 7) == 0 && mc_count(t) == 1 && !t->mc->nhistory);
    puts("PASS overlaps: replacement/removal unions, deduplicated line indent/Tab/Delete Line/comment, remove-empty-cursors, restored pre-removal carets and read-only protection");
}
static void mc_views_test(void)
{
    struct tab *t = mc_fixture("abc\nabc\n"); struct doc *d = t->doc;
    mc_key(K_DOWN, KEYS_CTRL | KEYS_ALT); CHECK(mc_count(t) == 2);
    split_direction(1); struct tab *other = cur_tab();
    CHECK(other->doc == d && mc_count(other) == 1);
    type_text(active, other, "Q", 1);
    active = 0; t = cur_tab();
    CHECK(mc_count(t) == 2); mc_check_caret(t, 0, 0); mc_check_caret(t, 5, 5);
    mc_key('z', KEYS_CTRL); CHECK(text_is(d, "abc\nabc\n"));
    mc_check_caret(t, 0, 0); mc_check_caret(t, 4, 4);
    mc_key('z', KEYS_CTRL | KEYS_SHIFT); CHECK(text_is(d, "abc\nQabc\n"));
    mc_check_caret(t, 0, 0); mc_check_caret(t, 5, 5);
    keys("X"); CHECK(text_is(d, "Xabc\nQXabc\n"));
    active = 1; other = cur_tab(); mc_key('z', KEYS_CTRL);
    CHECK(text_is(d, "abc\nQabc\n")); active = 0; t = cur_tab();
    mc_check_caret(t, 0, 0); mc_check_caret(t, 5, 5);
    /* Reallocation and array compaction retain owned state, never a tab* map. */
    for (int i = 0; i < 20; i++) new_document();
    show_doc(0, d); t = cur_tab(); CHECK(mc_count(t) == 2);
    drop_tab(0, 1); show_doc(0, d); CHECK(mc_count(cur_tab()) == 2);
    puts("PASS per-view isolation: split independence, external edit movement, undo from another view, tab realloc/compaction");
}
int main(void)
{
    CHECK(setlocale(LC_CTYPE, "C.UTF-8") != NULL);
    mc_creation_test(); mc_edit_test(); mc_newline_indent_test(); mc_unicode_test();
    mc_occurrence_test(); mc_block_test(); mc_block_mouse_tabs_test(); mc_overlap_test(); mc_views_test();
    mc_fixture("");
    for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) mc_free(&spaces[s].tabs[i]);
    reset(); edit_layout_free(&view_layout); free(docs); docs = NULL;
    puts("PASS all real editor multicursor and rectangular-selection fixtures");
    return 0;
}
