/* Per-view multiple cursors and rectangular selection (KatePart's defaults).
 * The implementation is included after edit.c's editing/key/pointer functions.
 * Keep this after edit-doc.h: only calls in the UI translation unit are wrapped,
 * never the document implementation itself. */
#ifndef LOCACHY_EDIT_MULTICURSOR_H
#define LOCACHY_EDIT_MULTICURSOR_H
struct tab;
struct pointer_event;
struct edit_multicursor;
static void mc_doc_begin(struct doc *, size_t, size_t);
static void mc_doc_end(struct doc *, size_t, size_t);
#define doc_begin(d,c,a) mc_doc_begin((d),(c),(a))
#define doc_end(d,c,a) mc_doc_end((d),(c),(a))
static int mc_type_text(int, struct tab *, const char *, size_t);
static int mc_newline(int, struct tab *, int);
static int mc_backspace(int, struct tab *);
static int mc_delete(int, struct tab *);
static int mc_delete_word(int, struct tab *, int);
static int mc_delete_lines(int, struct tab *);
static int mc_indent(struct tab *, int);
static int mc_tab(int, struct tab *);
static int mc_comment(int, struct tab *, int);
static int mc_to_bracket(int, struct tab *, int);
static int mc_copy(struct tab *, int, int);
static int mc_paste(int, struct tab *, char *, size_t);
static int mc_undo_redo(int, struct tab *, int);
static void mc_moved(struct tab *, size_t, size_t, size_t);
static int mc_action(int, struct tab *, int, int);
static int mc_view_key(int, struct tab *, int, int);
static int mc_click(int, struct tab *, const struct pointer_event *, size_t);
static int mc_drag(struct tab *, size_t);
static void mc_free(struct tab *);
static int mc_selected(const struct tab *, size_t);
static void mc_draw_carets(int, struct tab *, int, int, int, int, int);
static int mc_block(const struct tab *);
static size_t mc_count(const struct tab *);
static int mc_column(const struct doc *, size_t);
static void mc_add_vertical(int, struct tab *, int);
static void mc_line_ends(int, struct tab *);
static void mc_occurrence(int, struct tab *, int);
static void mc_skip(struct tab *);
static void mc_toggle_block(struct tab *);
static void mc_remove_empty(struct tab *) __attribute__((unused));
static void mc_clear(struct tab *);
static void mc_deselect(struct tab *);
static void mc_select_all(struct tab *);
static void view_key(int, struct tab *, int, int);
#endif
