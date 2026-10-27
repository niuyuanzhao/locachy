/* Per-view folding, included after struct tab (which owns `folding`).
 * Geometry stays in edit.c: move_rows/draw_text consume fold_next/prev,
 * jumps call fold_reveal, and a gutter click calls fold_toggle. This is
 * deliberately not document-global: split views can fold independently.
 * No backend node pointer survives a syntax edit; snapshots own their data. */
struct fold_hidden {
    size_t first, last, header;
};

struct edit_folding {
    const struct syntax *source;
    uint64_t epoch;
    int lang, live;
    struct syntax_fold *ranges;
    uint8_t *closed;
    size_t count;
    struct fold_hidden *hidden;
    size_t nhidden;
};

static inline void fold_release(struct tab *t)
{
    if (!t || !t->folding) return;
    free(t->folding->ranges);
    free(t->folding->closed);
    free(t->folding->hidden);
    free(t->folding);
    t->folding = NULL;
}

static inline void fold_hidden_rebuild(struct edit_folding *f)
{
    f->nhidden = 0;
    for (size_t i = 0; i < f->count; i++) {
        if (!f->closed[i]) continue;
        const struct syntax_fold *r = &f->ranges[i];
        size_t first = r->start_line + 1;
        if (f->nhidden && first <= f->hidden[f->nhidden - 1].last + 1) {
            if (r->end_line > f->hidden[f->nhidden - 1].last)
                f->hidden[f->nhidden - 1].last = r->end_line;
        } else {
            f->hidden[f->nhidden++] = (struct fold_hidden){ first, r->end_line, r->start_line };
        }
    }
}

/* const callers are geometry queries; the cache is logically mutable. A
 * dirty tree temporarily exposes all lines, never stale hidden endpoints.
 * Keep only the owned snapshot while parsing so unchanged regions can keep
 * their states when the completed tree arrives. */
static inline struct edit_folding *fold_sync(const struct tab *t)
{
    if (!t || !t->doc) return NULL;
    struct edit_folding *f = t->folding;
    struct syntax *s = t->doc->syntax;
    if (!s || syntax_off(s)) {
        if (f) f->live = 0;
        return f;
    }
    if (!f) {
        f = calloc(1, sizeof *f);
        if (!f) return NULL;
        ((struct tab *)t)->folding = f;
        f->lang = -1;
    }
    if (syntax_pending(s)) {
        f->live = 0;
        return f;
    }
    uint64_t epoch = syntax_epoch(s);
    if (f->source == s && f->lang == syntax_lang(s) && f->epoch == epoch && f->live) return f;
    const struct syntax_fold *ranges = NULL;
    size_t count = syntax_folds(s, &t->doc->buf, &ranges);
    struct syntax_fold *copy = count ? malloc(count * sizeof *copy) : NULL;
    uint8_t *closed = count ? calloc(count, sizeof *closed) : NULL;
    struct fold_hidden *hidden = count ? malloc(count * sizeof *hidden) : NULL;
    if (count && (!copy || !closed || !hidden)) {
        free(copy); free(closed); free(hidden);
        f->live = 0;
        return f;
    }
    if (count) memcpy(copy, ranges, count * sizeof *copy);
    if (f->source == s && f->lang == syntax_lang(s)) {
        size_t old = 0;
        for (size_t i = 0; i < count; i++) {
            while (old < f->count && f->ranges[old].start_byte < copy[i].start_byte) old++;
            if (old < f->count && f->ranges[old].start_byte == copy[i].start_byte &&
                f->ranges[old].end_byte == copy[i].end_byte) closed[i] = f->closed[old];
        }
    }
    free(f->ranges); free(f->closed); free(f->hidden);
    f->ranges = copy; f->closed = closed; f->hidden = hidden;
    f->count = count; f->nhidden = 0;
    f->source = s; f->epoch = epoch; f->lang = syntax_lang(s); f->live = 1;
    fold_hidden_rebuild(f);
    return f;
}

/* Hook beside moved_pos in the existing document moved callback. Position
 * mapping uses old byte endpoints only, never an invalidated backend array.
 * Replacing/removing the header destroys that region's collapsed state. */
static inline void fold_moved(struct tab *t, size_t at, size_t removed, size_t inserted)
{
    struct edit_folding *f = t ? t->folding : NULL;
    if (!f) return;
    for (size_t i = 0; i < f->count; i++) {
        struct syntax_fold *r = &f->ranges[i];
        if (removed && at <= r->start_byte && at + removed > r->start_byte) f->closed[i] = 0;
        size_t *points[] = { &r->start_byte, &r->end_byte };
        for (size_t p = 0; p < 2; p++) {
            size_t value = *points[p];
            if (value > at || (value == at && p == 0)) {
                *points[p] = value < at + removed ? at + inserted : value - removed + inserted;
            }
        }
    }
    f->live = 0;
}

static inline const struct fold_hidden *fold_hidden_at(const struct edit_folding *f, size_t line)
{
    if (!f || !f->live) return NULL;
    size_t lo = 0, hi = f->nhidden;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->hidden[mid].first <= line) lo = mid + 1;
        else hi = mid;
    }
    return lo && line <= f->hidden[lo - 1].last ? &f->hidden[lo - 1] : NULL;
}

/* A hidden input maps to its VISIBLE containing header, not its child.
 * fold_next returns buf.lines at EOF; fold_prev stays at line zero. */
static inline size_t fold_line(const struct tab *t, size_t line)
{
    struct edit_folding *f = fold_sync(t);
    const struct fold_hidden *h = fold_hidden_at(f, line);
    return h ? h->header : line;
}

static inline size_t fold_next(const struct tab *t, size_t line)
{
    size_t total = t->doc->buf.lines;
    if (line >= total) return total;
    struct edit_folding *f = fold_sync(t);
    const struct fold_hidden *h = fold_hidden_at(f, line + 1);
    return h ? (h->last + 1 < total ? h->last + 1 : total) : line + 1;
}

static inline size_t fold_prev(const struct tab *t, size_t line)
{
    if (!line) return 0;
    return fold_line(t, line - 1);
}

/* Bidirectional DOCUMENT line <-> visible logical-line index, used by
 * scrollbar/overview consumers; wrapping remains move_rows' responsibility.
 * A hidden document line shares the visible index of its fold header. */
static inline size_t fold_visible_index(const struct tab *t, size_t line)
{
    struct edit_folding *f = fold_sync(t);
    size_t index = fold_line(t, line);
    if (f && f->live) {
        for (size_t i = 0; i < f->nhidden && f->hidden[i].last < line; i++)
            index -= f->hidden[i].last - f->hidden[i].first + 1;
    }
    return index;
}

static inline size_t fold_document_line(const struct tab *t, size_t index)
{
    struct edit_folding *f = fold_sync(t);
    size_t line = index;
    if (f && f->live) {
        for (size_t i = 0; i < f->nhidden; i++) {
            if (line < f->hidden[i].first) break;
            line += f->hidden[i].last - f->hidden[i].first + 1;
        }
    }
    return line < t->doc->buf.lines ? line : t->doc->buf.lines - 1;
}

static inline size_t fold_start_index(const struct edit_folding *f, size_t line)
{
    size_t lo = 0, hi = f && f->live ? f->count : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->ranges[mid].start_line < line) lo = mid + 1;
        else hi = mid;
    }
    return f && f->live && lo < f->count && f->ranges[lo].start_line == line ? lo : SIZE_MAX;
}

/* '+' collapsed, '-' expanded, 0 no region. Wrapped continuation rows must
 * not draw a second marker for the same document line. */
static inline int fold_marker(const struct tab *t, size_t line)
{
    struct edit_folding *f = fold_sync(t);
    size_t i = fold_start_index(f, line);
    return i == SIZE_MAX ? 0 : f->closed[i] ? '+' : '-';
}

/* Collapse never leaves the caret/top inside invisible text. Do not call
 * show_cursor here: a jump-reveal hook would immediately undo the action. */
static inline void fold_adjust(struct tab *t)
{
    size_t line = buffer_line_of(&t->doc->buf, t->cursor), visible = fold_line(t, line);
    if (line != visible) t->cursor = buffer_line_start(&t->doc->buf, visible);
    line = buffer_line_of(&t->doc->buf, t->anchor);
    visible = fold_line(t, line);
    if (line != visible) t->anchor = buffer_line_start(&t->doc->buf, visible);
    line = buffer_line_of(&t->doc->buf, t->top);
    visible = fold_line(t, line);
    if (line != visible) {
        t->top = buffer_line_start(&t->doc->buf, visible);
        t->sub = 0;
    }
    t->goal = -1;
}

static inline int fold_set_index(struct tab *t, struct edit_folding *f, size_t index, int collapse, int recursive)
{
    if (!f || !f->live || index >= f->count) return 0;
    int changed = f->closed[index] != !!collapse;
    f->closed[index] = !!collapse;
    for (size_t i = index + 1; recursive && i < f->count && f->ranges[i].depth > f->ranges[index].depth; i++) {
        changed |= f->closed[i] != !!collapse;
        f->closed[i] = !!collapse;
    }
    fold_hidden_rebuild(f);
    fold_adjust(t);
    return changed;
}

static inline int fold_toggle(struct tab *t, size_t line, int recursive)
{
    struct edit_folding *f = fold_sync(t);
    size_t i = fold_start_index(f, line);
    return i == SIZE_MAX ? 0 : fold_set_index(t, f, i, !f->closed[i], recursive);
}

static inline int fold_current(struct tab *t, int collapse, int recursive)
{
    struct edit_folding *f = fold_sync(t);
    if (!f || !f->live) return 0;
    size_t line = buffer_line_of(&t->doc->buf, t->cursor);
    size_t i = syntax_fold_at(f->ranges, f->count, line);
    return i == SIZE_MAX ? 0 : fold_set_index(t, f, i, collapse, recursive);
}

static inline int fold_top_level(struct tab *t, int collapse, int recursive)
{
    struct edit_folding *f = fold_sync(t);
    if (!f || !f->live) return 0;
    int changed = 0;
    for (size_t i = 0; i < f->count; i++) {
        if (recursive || f->ranges[i].parent == SIZE_MAX) {
            changed |= f->closed[i] != !!collapse;
            f->closed[i] = !!collapse;
        }
    }
    fold_hidden_rebuild(f);
    fold_adjust(t);
    return changed;
}

/* Search/goto/direct cursor positioning reveals exactly the ancestors
 * hiding the target, leaving unrelated and visible-header child folds alone. */
static inline int fold_reveal(struct tab *t, size_t line)
{
    struct edit_folding *f = fold_sync(t);
    if (!f || !f->live) return 0;
    int changed = 0;
    for (size_t i = 0; i < f->count; i++) {
        if (f->closed[i] && f->ranges[i].start_line < line && line <= f->ranges[i].end_line) {
            f->closed[i] = 0;
            changed = 1;
        }
    }
    if (changed) fold_hidden_rebuild(f);
    return changed;
}
