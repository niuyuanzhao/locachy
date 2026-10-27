/*
 * edit's documents (edit-doc.h). Undo follows KTextEditor's KateUndoManager
 * (src/undo/kateundomanager.cpp, kateundo.cpp): an edit (editStart ..
 * editEnd) is a group of items; a new group joins the last one unless that
 * one is at a safe point, and only when every item of both is of one kind
 * (typing text, removing text, a line break, joining lines), so typed
 * characters are one step, and typing after Backspace or Enter starts
 * another. A safe point is set when the cursor moves other than by an edit,
 * when the document is saved, around a paste, and when undo or redo comes
 * back to the saved text. Undo puts back the cursor and selection from
 * before the group, redo those after it. A new edit drops what could be
 * redone. The document is unmodified again when undo or redo reach the
 * state it was saved in.
 */
#include <stdlib.h>
#include <string.h>

#include "edit-doc.h"

/* The kinds of item, for merging (Kate's editInsertText, editRemoveText,
 * editWrapLine, editUnWrapLine; text with line breaks in it is several). */
enum { KIND_INSERT, KIND_REMOVE, KIND_WRAP, KIND_UNWRAP, KIND_MIXED };

static int kind_of(const struct undo_item *it)
{
    if (it->type == UNDO_MANY) {
        return KIND_MIXED;
    }
    int nl = memchr(it->text, '\n', it->len) != NULL;
    if (!nl) {
        return it->type == UNDO_INSERT ? KIND_INSERT : KIND_REMOVE;
    }
    if (it->len == 1) {
        return it->type == UNDO_INSERT ? KIND_WRAP : KIND_UNWRAP;
    }
    return KIND_MIXED;
}

/* KateUndoGroup::singleType: the kind of all items, or KIND_MIXED. */
static int group_kind(const struct undo_group *g)
{
    int k = -1;
    for (size_t i = 0; i < g->n; i++) {
        int ki = kind_of(&g->items[i]);
        if (ki == KIND_MIXED || (k >= 0 && ki != k)) {
            return KIND_MIXED;
        }
        k = ki;
    }
    return k < 0 ? KIND_MIXED : k;
}

static void free_item(struct undo_item *it)
{
    free(it->text);
    if (it->r) {
        for (size_t i = 0; i < it->n; i++) {
            free((char *)it->r[i].text);
        }
        free(it->r);
    }
}

static void free_group(struct undo_group *g)
{
    for (size_t i = 0; i < g->n; i++) {
        free_item(&g->items[i]);
    }
    free(g->items);
    memset(g, 0, sizeof *g);
}

int doc_init(struct doc *d)
{
    memset(d, 0, sizeof *d);
    d->lang = -1;
    d->watch = -1;
    d->saved = 0;
    return buffer_init(&d->buf);
}

static void drop_redo(struct doc *d)
{
    for (size_t i = d->done; i < d->count; i++) {
        free_group(&d->groups[i]);
    }
    if (d->saved > (long)d->done) {
        d->saved = -1; /* the saved state can no longer be reached */
    }
    d->count = d->done;
}

static void clear_undo(struct doc *d)
{
    for (size_t i = 0; i < d->count; i++) {
        free_group(&d->groups[i]);
    }
    d->done = d->count = 0;
    d->saved = 0;
}

void doc_free(struct doc *d)
{
    clear_undo(d);
    free(d->groups);
    if (d->open) {
        free_group(d->open);
        free(d->open);
    }
    syntax_free(d->syntax);
    buffer_free(&d->buf);
    memset(d, 0, sizeof *d);
}

int doc_set_text(struct doc *d, const char *text, size_t len)
{
    size_t old = buffer_length(&d->buf);
    if (buffer_set(&d->buf, text, len) < 0) {
        return -1;
    }
    clear_undo(d);
    syntax_reset(d->syntax);
    if (d->moved) {
        d->moved(d, 0, old, len);
    }
    return 0;
}

int doc_take_text(struct doc *d, char *text, size_t len)
{
    size_t old = buffer_length(&d->buf);
    if (buffer_take(&d->buf, text, len) < 0) {
        return -1;
    }
    clear_undo(d);
    syntax_reset(d->syntax);
    if (d->moved) {
        d->moved(d, 0, old, len);
    }
    return 0;
}

int doc_modified(const struct doc *d)
{
    return d->saved != (long)d->done;
}

void doc_mark_saved(struct doc *d)
{
    d->saved = (long)d->done;
    doc_safe_point(d);
}

void doc_detect_syntax(struct doc *d)
{
    int lang = syntax_detect(d->path[0] ? d->path : NULL, &d->buf);
    if (lang == d->lang && (d->syntax || lang < 0)) {
        return;
    }
    syntax_free(d->syntax);
    d->syntax = NULL;
    d->syntax_why[0] = '\0';
    d->lang = lang;
    if (lang >= 0) {
        d->syntax = syntax_new(lang, d->syntax_why, sizeof d->syntax_why);
    }
}

void doc_line_col(const struct doc *d, size_t at, size_t *line, size_t *col)
{
    *line = buffer_line_of(&d->buf, at);
    *col = at - buffer_line_start(&d->buf, *line);
}

void doc_begin(struct doc *d, size_t cursor, size_t anchor)
{
    if (d->open) {
        return;
    }
    d->open = calloc(1, sizeof *d->open);
    if (d->open) {
        d->open->cursor_before = cursor;
        d->open->anchor_before = anchor;
    }
}

/* Kate's mergeUndoItems: typing on one line, and Backspace over it. */
static int merge_item(struct undo_item *base, const struct undo_item *u)
{
    if (base->type != u->type || base->type == UNDO_MANY || memchr(u->text, '\n', u->len) ||
        memchr(base->text, '\n', base->len)) {
        return 0;
    }
    if (base->type == UNDO_INSERT && base->at + base->len == u->at) {
        char *t = realloc(base->text, base->len + u->len);
        if (!t) {
            return 0;
        }
        memcpy(t + base->len, u->text, u->len);
        base->text = t;
        base->len += u->len;
        return 1;
    }
    if (base->type == UNDO_REMOVE && base->at == u->at + u->len) {
        char *t = malloc(base->len + u->len);
        if (!t) {
            return 0;
        }
        memcpy(t, u->text, u->len);
        memcpy(t + u->len, base->text, base->len);
        free(base->text);
        base->text = t;
        base->at = u->at;
        base->len += u->len;
        return 1;
    }
    return 0;
}

static int add_item(struct undo_group *g, struct undo_item *it)
{
    if (g->n > 0 && merge_item(&g->items[g->n - 1], it)) {
        free_item(it);
        return 0;
    }
    if (g->n == g->size) {
        size_t size = g->size ? g->size * 2 : 4;
        struct undo_item *items = realloc(g->items, size * sizeof *items);
        if (!items) {
            free_item(it);
            return -1;
        }
        g->items = items;
        g->size = size;
    }
    g->items[g->n++] = *it;
    return 0;
}

static int push_group(struct doc *d, struct undo_group *g)
{
    if (d->count == d->size) {
        size_t size = d->size ? d->size * 2 : 64;
        struct undo_group *groups = realloc(d->groups, size * sizeof *groups);
        if (!groups) {
            return -1;
        }
        d->groups = groups;
        d->size = size;
    }
    d->groups[d->count++] = *g;
    d->done = d->count;
    return 0;
}

void doc_end(struct doc *d, size_t cursor, size_t anchor)
{
    struct undo_group *g = d->open;
    d->open = NULL;
    if (!g) {
        return;
    }
    g->cursor_after = cursor;
    g->anchor_after = anchor;
    if (g->n == 0) {
        free(g);
        return;
    }
    drop_redo(d);
    struct undo_group *last = d->done > 0 ? &d->groups[d->done - 1] : NULL;
    int k = last ? group_kind(last) : KIND_MIXED;
    /* KateUndoGroup::merge: not past a safe point, and only one kind. */
    if (last && !last->safe && k != KIND_MIXED && group_kind(g) == k) {
        for (size_t i = 0; i < g->n; i++) {
            add_item(last, &g->items[i]);
        }
        free(g->items);
        last->cursor_after = cursor;
        last->anchor_after = anchor;
        last->safe |= g->safe;
        free(g);
        return;
    }
    if (push_group(d, g) < 0) {
        free_group(g);
    }
    free(g);
}

void doc_safe_point(struct doc *d)
{
    if (d->open) {
        d->open->safe = 1;
    } else if (d->done > 0) {
        d->groups[d->done - 1].safe = 1;
    }
}

/* The line and byte in it after text is inserted at (line, col). */
static void end_of(const char *text, size_t len, size_t line, size_t col, size_t *eline, size_t *ecol)
{
    *eline = line;
    *ecol = col;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            (*eline)++;
            *ecol = 0;
        } else {
            (*ecol)++;
        }
    }
}

/* The text changes, without undo. */
static int raw_insert(struct doc *d, size_t at, const char *text, size_t len)
{
    size_t line, col, eline, ecol;
    doc_line_col(d, at, &line, &col);
    if (buffer_insert(&d->buf, at, text, len) < 0) {
        return -1;
    }
    end_of(text, len, line, col, &eline, &ecol);
    syntax_edit(d->syntax, at, at, at + len, line, col, line, col, eline, ecol);
    if (d->moved) {
        d->moved(d, at, 0, len);
    }
    return 0;
}

static void raw_remove(struct doc *d, size_t at, size_t len)
{
    size_t line, col, eline, ecol;
    doc_line_col(d, at, &line, &col);
    doc_line_col(d, at + len, &eline, &ecol);
    buffer_delete(&d->buf, at, len);
    syntax_edit(d->syntax, at, at + len, at, line, col, eline, ecol, line, col);
    if (d->moved) {
        d->moved(d, at, len, 0);
    }
}

/* r applied: each r[i].at, len and text are of the text before; after it
 * they are made those of the text after, with the replaced text in text,
 * so that the same call undoes it. */
static int raw_many(struct doc *d, struct replacement *r, size_t n)
{
    char **old = malloc((n ? n : 1) * sizeof *old);
    if (!old) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        old[i] = buffer_dup(&d->buf, r[i].at, r[i].len);
        if (!old[i]) {
            while (i > 0) {
                free(old[--i]);
            }
            free(old);
            return -1;
        }
    }
    if (buffer_replace_many(&d->buf, r, n) < 0) {
        for (size_t i = 0; i < n; i++) {
            free(old[i]);
        }
        free(old);
        return -1;
    }
    long shift = 0;
    for (size_t i = 0; i < n; i++) {
        size_t new_at = (size_t)((long)r[i].at + shift), new_len = r[i].new_len, old_len = r[i].len;
        shift += (long)new_len - (long)old_len;
        free((char *)r[i].text);
        r[i] = (struct replacement){ new_at, new_len, old[i], old_len };
    }
    free(old);
    syntax_reset(d->syntax);
    /* The positions of views move as by each replacement in turn. */
    for (size_t i = 0; d->moved && i < n; i++) {
        d->moved(d, r[i].at, r[i].new_len, r[i].len);
    }
    return 0;
}

int doc_insert(struct doc *d, size_t at, const char *text, size_t len)
{
    if (len == 0) {
        return 0;
    }
    int alone = !d->open;
    if (alone) {
        doc_begin(d, at, at);
    }
    struct undo_item it = { UNDO_INSERT, at, len, malloc(len), NULL, 0 };
    if (!it.text || raw_insert(d, at, text, len) < 0) {
        free(it.text);
        if (alone) {
            doc_end(d, at, at);
        }
        return -1;
    }
    memcpy(it.text, text, len);
    if (d->open) {
        add_item(d->open, &it);
    } else {
        free(it.text);
    }
    if (alone) {
        doc_end(d, at + len, at + len);
    }
    return 0;
}

void doc_remove(struct doc *d, size_t at, size_t len)
{
    size_t total = buffer_length(&d->buf);
    if (at >= total || len == 0) {
        return;
    }
    len = len > total - at ? total - at : len;
    int alone = !d->open;
    if (alone) {
        doc_begin(d, at + len, at + len);
    }
    struct undo_item it = { UNDO_REMOVE, at, len, buffer_dup(&d->buf, at, len), NULL, 0 };
    if (it.text) {
        raw_remove(d, at, len);
        if (d->open) {
            add_item(d->open, &it);
        } else {
            free(it.text);
        }
    }
    if (alone) {
        doc_end(d, at, at);
    }
}

int doc_replace_many(struct doc *d, struct replacement *r, size_t n)
{
    if (n == 0) {
        return 0;
    }
    /* Each text is the caller's: copies, which the undo item keeps. */
    for (size_t i = 0; i < n; i++) {
        char *t = malloc(r[i].new_len ? r[i].new_len : 1);
        if (!t) {
            while (i > 0) {
                free((char *)r[--i].text);
            }
            return -1;
        }
        memcpy(t, r[i].text, r[i].new_len);
        r[i].text = t;
    }
    int alone = !d->open;
    if (alone) {
        doc_begin(d, r[0].at, r[0].at);
    }
    struct replacement *copy = malloc(n * sizeof *copy);
    if (!copy || raw_many(d, r, n) < 0) {
        free(copy);
        for (size_t i = 0; i < n; i++) {
            free((char *)r[i].text);
        }
        if (alone) {
            doc_end(d, r[0].at, r[0].at);
        }
        return -1;
    }
    memcpy(copy, r, n * sizeof *copy);
    struct undo_item it = { UNDO_MANY, 0, 0, NULL, copy, n };
    if (d->open) {
        add_item(d->open, &it);
    } else {
        free_item(&it);
    }
    if (alone) {
        doc_end(d, r[n - 1].at + r[n - 1].len, r[n - 1].at + r[n - 1].len);
    }
    return 0;
}

static void apply(struct doc *d, struct undo_item *it, int undo)
{
    if (it->type == UNDO_MANY) {
        raw_many(d, it->r, it->n);
    } else if ((it->type == UNDO_INSERT) == !undo) {
        raw_insert(d, it->at, it->text, it->len);
    } else {
        raw_remove(d, it->at, it->len);
    }
}

/* Back at the saved state: unmodified, and the next edit does not join. */
static void check_saved(struct doc *d)
{
    if ((long)d->done == d->saved) {
        doc_safe_point(d);
    }
}

int doc_undo(struct doc *d, size_t *cursor, size_t *anchor)
{
    if (d->open || d->done == 0) {
        return 0;
    }
    struct undo_group *g = &d->groups[--d->done];
    for (size_t i = g->n; i > 0; i--) {
        apply(d, &g->items[i - 1], 1);
    }
    *cursor = g->cursor_before;
    *anchor = g->anchor_before;
    check_saved(d);
    return 1;
}

int doc_redo(struct doc *d, size_t *cursor, size_t *anchor)
{
    if (d->open || d->done == d->count) {
        return 0;
    }
    struct undo_group *g = &d->groups[d->done++];
    for (size_t i = 0; i < g->n; i++) {
        apply(d, &g->items[i], 0);
    }
    *cursor = g->cursor_after;
    *anchor = g->anchor_after;
    check_saved(d);
    return 1;
}
