/* edit's syntax highlighting (edit-syntax.c): tree-sitter, with the grammars
 * as packaged (/usr/lib/libtree-sitter-LANG.so, opened when a document needs
 * one) and their highlights.scm queries: the package's, else the copy that
 * comes with edit (EDIT_QUERIES/LANG/highlights.scm). */
#ifndef LOCACHY_EDIT_SYNTAX_H
#define LOCACHY_EDIT_SYNTAX_H

#include <stddef.h>
#include <stdint.h>

#include "edit-buffer.h"

#ifndef EDIT_QUERIES
#define EDIT_QUERIES "/usr/local/share/locachy/edit/queries"
#endif
#ifndef EDIT_GRAMMARS
#define EDIT_GRAMMARS "/usr/lib"
#endif
#ifndef PACKAGED_QUERIES
#define PACKAGED_QUERIES "/usr/share/tree-sitter/queries"
#endif

/* The colours of the text (Kate's Breeze Dark theme). */
#define COLOUR_NORMAL 0xCFCFC2

struct syntax;

/* The language for a file: by its name, its first line (#!) or a modeline
 * (kate: hl NAME; or vim's ft=) in its first or last lines; -1 if none. */
int syntax_detect(const char *path, const struct buffer *b);
/* The language of Kate's name for it (as in "kate: hl C++;"), or -1. */
int syntax_by_name(const char *name);
const char *syntax_name(int lang); /* "C++", ...; "Normal" for -1 */
int syntax_count(void);

/* A document's highlighting in language lang: NULL, and why in why, if its
 * grammar or query cannot be used. */
struct syntax *syntax_new(int lang, char *why, size_t size);
void syntax_free(struct syntax *s);
int syntax_lang(const struct syntax *s);

/* An edit of the text (bytes, and lines with the bytes into them) since the
 * last parse; syntax_reset: all of it changed. */
void syntax_edit(struct syntax *s, size_t start, size_t old_end, size_t new_end, size_t start_line,
                 size_t start_col, size_t old_end_line, size_t old_end_col, size_t new_end_line,
                 size_t new_end_col);
void syntax_reset(struct syntax *s);
/* Parse what changed, for at most budget_ms milliseconds (0: to the end):
 * 1 when done, 0 when there is more to do (call it again, with the same
 * text), -1 when the text is past what is highlighted (SYNTAX_LIMIT). */
int syntax_parse(struct syntax *s, struct buffer *b, int budget_ms);
/* A parse is under way (syntax_parse returned 0 for it). */
int syntax_pending(const struct syntax *s);
/* The colour of each byte of [start, end) of b, into colour (end - start of
 * them), by the query's captures of the nodes there (the innermost node,
 * and for one node its last pattern, as tree-sitter's highlighter), from
 * the last finished parse. */
void syntax_colours(struct syntax *s, struct buffer *b, size_t start, size_t end, uint32_t *colour);
/* Font metadata, independently of the existing foreground colours. Bits
 * follow Breeze Dark's default styles and Kate's Markdown overrides.
 * Comment/Documentation are NOT italic in Breeze Dark. */
enum syntax_style {
    SYNTAX_STYLE_BOLD = 1,
    SYNTAX_STYLE_ITALIC = 2,
    SYNTAX_STYLE_UNDERLINE = 4,
};
void syntax_styles(struct syntax *s, struct buffer *b, size_t start, size_t end, uint8_t *styles);

/* A fold's actual syntax-node bytes [start_byte, end_byte), and zero-based
 * occupied lines (end_line inclusive; an end at column zero belongs to the
 * previous line). Collapse hides start_line + 1 through end_line, NEVER the
 * start line. Only multi-line regions are returned. Parent indexes refer to
 * this array; SIZE_MAX means top-level, whose depth is zero. Regions are
 * sorted by start byte, outer first, without duplicate line ranges or
 * crossing line ranges. Use the parent chain for recursive operations;
 * non-recursive unfold must leave children's collapsed state untouched. */
struct syntax_fold {
    size_t start_byte, end_byte;
    size_t start_line, end_line;
    size_t parent;
    unsigned depth;
};
/* The immutable array belongs to s, valid until edit/reset/parse/free. Empty
 * (and *out NULL) while dirty, off, absent, or after allocation failure.
 * This does not parse: call syntax_parse with the UI's normal time budget.
 * No indentation/brace guess is made for Normal or an unavailable grammar. */
size_t syntax_folds(struct syntax *s, struct buffer *b, const struct syntax_fold **out);
/* Changes on edit/reset and completed parse, including an empty fold tree.
 * A cached UI index/selection must not survive an epoch change unchecked. */
uint64_t syntax_epoch(const struct syntax *s);
/* Innermost region containing a line, or SIZE_MAX; current-node commands
 * use this, while top-level commands select parent == SIZE_MAX. */
size_t syntax_fold_at(const struct syntax_fold *folds, size_t count, size_t line);
/* The capture name that gave the colour of byte at ("" if none), for the
 * tests and the status of a theme. */
const char *syntax_capture_at(struct syntax *s, struct buffer *b, size_t at);
/* Why highlighting is off for this document, or NULL. */
const char *syntax_off(const struct syntax *s);

/* The comment marks of a language, as Kate's syntax definitions give them,
 * for comment and uncomment (Ctrl+D): the single-line one ("//"), whether it
 * goes after the indentation (position="afterwhitespace"), and the start
 * and end of a multi-line one; NULL where it has none. */
void syntax_comment_marks(int lang, const char **line, int *after_space, const char **start, const char **end);

#endif
