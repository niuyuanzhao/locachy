/* KatePart multiple cursors and block selection. Included in edit.c, not a
 * separate editor/backend: the original editing and navigation functions run
 * once for each distinct caret, inside one document edit/undo transaction. */
struct mc_caret {
    size_t cursor, anchor;
    int goal, column, anchor_column;
};
struct mc_snapshot {
    struct mc_caret *carets;
    size_t n;
    int block, rectangle, acol, ccol;
    size_t aline, cline;
};
struct mc_history {
    size_t group;
    const struct undo_item *token;
    struct mc_snapshot before, after;
};
struct edit_multicursor {
    struct mc_caret *carets;
    size_t n, capacity, current;
    int busy, block, rectangle, acol, ccol;
    size_t aline, cline;
    struct mc_history *history;
    size_t nhistory;
    char *needle;
    size_t needle_len;
    int skip;
};
static struct doc *mc_transaction_doc;

static void mc_forget_history(struct edit_multicursor *m);
static struct mc_caret mc_primary(const struct tab *t)
{
    return (struct mc_caret){ t->cursor, t->anchor, t->goal, -1, -1 };
}
static void mc_put_primary(struct tab *t, const struct mc_caret *c)
{
    t->cursor = c->cursor;
    t->anchor = c->anchor;
    t->goal = c->goal;
}
static int mc_reserve(struct edit_multicursor *m, size_t n)
{
    if (n <= m->capacity) return 1;
    size_t cap = m->capacity ? m->capacity : 4;
    while (cap < n && cap <= SIZE_MAX / 2) cap *= 2;
    if (cap < n || cap > SIZE_MAX / sizeof *m->carets) return 0;
    struct mc_caret *p = realloc(m->carets, cap * sizeof *p);
    if (!p) return 0;
    m->carets = p;
    m->capacity = cap;
    return 1;
}
static struct edit_multicursor *mc_state(struct tab *t)
{
    if (!t->mc) {
        t->mc = calloc(1, sizeof *t->mc);
        if (!t->mc) return NULL;
        if (!mc_reserve(t->mc, 1)) { free(t->mc); t->mc = NULL; return NULL; }
        t->mc->n = 1;
        t->mc->carets[0] = mc_primary(t);
    }
    if (!t->mc->busy) {
        int col = t->mc->carets[0].column, anchor_col = t->mc->carets[0].anchor_column;
        t->mc->carets[0] = mc_primary(t);
        t->mc->carets[0].column = col;
        t->mc->carets[0].anchor_column = anchor_col;
    }
    return t->mc;
}
static size_t mc_count(const struct tab *t) { return t && t->mc ? t->mc->n : 1; }
static int mc_block(const struct tab *t) { return t && t->mc && t->mc->block; }
static void mc_forget_history(struct edit_multicursor *m)
{
    for (size_t i = 0; i < m->nhistory; i++) {
        free(m->history[i].before.carets);
        free(m->history[i].after.carets);
    }
    free(m->history); m->history = NULL; m->nhistory = 0;
}
static void mc_snapshot_free(struct mc_snapshot *s)
{
    free(s->carets);
    memset(s, 0, sizeof *s);
}
static void mc_history_free(struct mc_history *h)
{
    mc_snapshot_free(&h->before);
    mc_snapshot_free(&h->after);
}
static void mc_free(struct tab *t)
{
    struct edit_multicursor *m = t->mc;
    if (!m) return;
    for (size_t i = 0; i < m->nhistory; i++) mc_history_free(&m->history[i]);
    free(m->history);
    free(m->needle);
    free(m->carets);
    free(m);
    t->mc = NULL;
}
static void mc_clear(struct tab *t)
{
    if (!t || !t->mc) return;
    struct edit_multicursor *m = t->mc;
    m->n = 1;
    m->rectangle = 0;
    m->carets[0] = mc_primary(t);
    free(m->needle);
    m->needle = NULL;
    m->needle_len = 0;
    m->skip = 0;
}
static void mc_deselect(struct tab *t)
{
    if (!t) return;
    struct edit_multicursor *m = mc_state(t);
    t->anchor = t->cursor;
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) m->carets[i].anchor = m->carets[i].cursor;
    m->rectangle = 0;
    m->aline = m->cline = line_of(t->doc, t->cursor);
    m->acol = m->ccol = mc_column(t->doc, t->cursor);
}
static void mc_select_all(struct tab *t)
{
    if (!t) return;
    mc_clear(t);
    t->anchor = 0;
    t->cursor = doc_len(t->doc);
    set_primary(t);
}
static int mc_active(const struct tab *t)
{
    return t && t->mc && !t->mc->busy && (t->mc->n > 1 || t->mc->rectangle);
}
static void mc_remove_at(struct edit_multicursor *m, size_t i)
{
    memmove(m->carets + i, m->carets + i + 1, (m->n - i - 1) * sizeof *m->carets);
    m->n--;
}
/* Collapse duplicate carets and overlapping selections BEFORE applying edits.
 * Adjacent ranges stay separate: replacing adjacent selected words is valid. */
static void mc_normalize(struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) {
        size_t a = min_z(m->carets[i].cursor, m->carets[i].anchor);
        size_t b = max_z(m->carets[i].cursor, m->carets[i].anchor);
        for (size_t j = i + 1; j < m->n;) {
            size_t c = min_z(m->carets[j].cursor, m->carets[j].anchor);
            size_t d = max_z(m->carets[j].cursor, m->carets[j].anchor);
            if ((a == c && b == d) || (a < d && c < b) ||
                (a == b && c <= a && a < d) || (c == d && a <= c && c < b)) {
                a = min_z(a, c); b = max_z(b, d);
                int forward = m->carets[i].cursor >= m->carets[i].anchor;
                m->carets[i].anchor = forward ? a : b;
                m->carets[i].cursor = forward ? b : a;
                mc_remove_at(m, j);
            } else j++;
        }
    }
    mc_put_primary(t, m->carets);
}
static int mc_add(struct tab *t, struct mc_caret c, int make_primary)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return 0;
    for (size_t i = 0; i < m->n; i++) {
        if (m->carets[i].cursor == c.cursor && m->carets[i].anchor == c.anchor) {
            if (make_primary && i) {
                struct mc_caret old = m->carets[0];
                m->carets[0] = m->carets[i]; m->carets[i] = old;
                mc_put_primary(t, m->carets);
            }
            return 0;
        }
    }
    if (!mc_reserve(m, m->n + 1)) return 0;
    if (make_primary) {
        m->carets[m->n++] = m->carets[0]; m->carets[0] = c;
        mc_put_primary(t, m->carets);
    } else m->carets[m->n++] = c;
    return 1;
}
static void mc_moved(struct tab *t, size_t at, size_t removed, size_t inserted)
{
    struct edit_multicursor *m = t->mc;
    if (!m) return;
    if (!t->doc->open && !t->doc->done && !t->doc->count) {
        /* Loading/reloading replaces the buffer and clears document undo. */
        mc_forget_history(m); m->n = 1; m->rectangle = 0;
        m->carets[0].column = m->carets[0].anchor_column = -1;
        free(m->needle); m->needle = NULL; m->needle_len = 0;
    }
    for (size_t i = m->busy ? 0 : 1; i < m->n; i++) {
        if (m->busy && i == m->current) continue;
        struct mc_caret *c = &m->carets[i];
        int sel = c->cursor != c->anchor;
        c->cursor = moved_pos(c->cursor, at, removed, inserted, 1);
        c->anchor = sel ? moved_pos(c->anchor, at, removed, inserted, 0) : c->cursor;
    }
}
static int mc_snapshot_take(struct tab *t, struct mc_snapshot *out)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return 0;
    *out = (struct mc_snapshot){ .n = m->n, .block = m->block, .rectangle = m->rectangle,
        .acol = m->acol, .ccol = m->ccol, .aline = m->aline, .cline = m->cline };
    out->carets = malloc(m->n * sizeof *out->carets);
    if (!out->carets) return 0;
    memcpy(out->carets, m->carets, m->n * sizeof *out->carets);
    return 1;
}
static void mc_snapshot_restore(struct tab *t, const struct mc_snapshot *snap)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m || !snap->n || !mc_reserve(m, snap->n)) return;
    memcpy(m->carets, snap->carets, snap->n * sizeof *m->carets);
    m->n = snap->n; m->block = snap->block; m->rectangle = snap->rectangle;
    m->acol = snap->acol; m->ccol = snap->ccol; m->aline = snap->aline; m->cline = snap->cline;
    mc_put_primary(t, m->carets);
}
/* Screen/layout columns, never UTF-8 bytes. Tabs use the same stops and
 * grapheme widths as edit.c's renderer and hit testing. */
static int mc_column(const struct doc *d, size_t at)
{
    return cells_between(d, line_start(d, line_of(d, at)), at);
}
static size_t mc_at_column(const struct doc *d, size_t line, int col)
{
    int x = 0;
    size_t end = line_end(d, line);
    for (size_t p = line_start(d, line); p < end;) {
        int w; uint32_t cp;
        size_t next = cluster(d, p, end, x, &w, &cp);
        if (col < x + w) return col - x <= x + w - col ? p : next;
        x += w; p = next;
    }
    return end;
}
static void mc_rectangle(struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    size_t first = min_z(m->aline, m->cline), last = max_z(m->aline, m->cline);
    if (!mc_reserve(m, last - first + 1)) return;
    m->n = 0;
    for (size_t line = first; line <= last; line++) {
        struct mc_caret c = { mc_at_column(t->doc, line, m->ccol), mc_at_column(t->doc, line, m->acol),
                               -1, m->ccol, m->acol };
        if (line == m->cline) {
            if (m->n) m->carets[m->n] = m->carets[0];
            m->carets[0] = c;
        } else m->carets[m->n] = c;
        m->n++;
    }
    m->rectangle = 1;
    mc_put_primary(t, m->carets);
}
static void mc_toggle_block(struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    m->block = !m->block;
    if (m->block) {
        m->aline = line_of(t->doc, t->anchor); m->cline = line_of(t->doc, t->cursor);
        m->acol = mc_column(t->doc, t->anchor); m->ccol = mc_column(t->doc, t->cursor);
        mc_rectangle(t);
    } else mc_clear(t);
    doc_safe_point(t->doc);
}
static void mc_add_vertical(int s, struct tab *t, int dir)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    size_t edge = t->cursor;
    for (size_t i = 1; i < m->n; i++) {
        if ((dir < 0 && m->carets[i].cursor < edge) || (dir > 0 && m->carets[i].cursor > edge)) edge = m->carets[i].cursor;
    }
    size_t line = line_of(t->doc, edge);
    if ((dir < 0 && !line) || (dir > 0 && line + 1 >= t->doc->buf.lines)) return;
    int column = m->carets[0].column >= 0 ? m->carets[0].column : mc_column(t->doc, t->cursor);
    size_t at = mc_at_column(t->doc, dir < 0 ? line - 1 : line + 1, column);
    mc_add(t, (struct mc_caret){ at, at, -1, column, column }, 1);
    doc_safe_point(t->doc);
    show_cursor(s, t, 0);
}
static void mc_line_ends(int s, struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    int selection = 0;
    for (size_t i = 0; i < m->n; i++) if (m->carets[i].cursor != m->carets[i].anchor) selection = 1;
    if (!selection && !m->rectangle) return;
    size_t primary_line = line_of(t->doc, t->cursor);
    size_t n = m->n;
    struct mc_caret *original = malloc(n * sizeof *original);
    if (!original) return;
    memcpy(original, m->carets, n * sizeof *original);
    m->n = 0; m->rectangle = 0;
    for (size_t i = 0; i < n; i++) {
        size_t a = min_z(original[i].cursor, original[i].anchor), b = max_z(original[i].cursor, original[i].anchor);
        size_t first = line_of(t->doc, a), last = line_of(t->doc, b);
        for (size_t line = first; line <= last; line++) {
            size_t at = line_end(t->doc, line);
            if (!mc_reserve(m, m->n + 1)) break;
            m->carets[m->n++] = (struct mc_caret){ at, at, -1, -1, -1 };
        }
    }
    free(original);
    if (!m->n) { m->n = 1; m->carets[0] = mc_primary(t); }
    for (size_t i = 0; i < m->n; i++) if (line_of(t->doc, m->carets[i].cursor) == primary_line) {
        struct mc_caret old = m->carets[0]; m->carets[0] = m->carets[i]; m->carets[i] = old; break;
    }
    mc_put_primary(t, m->carets);
    mc_normalize(t);
    doc_safe_point(t->doc); show_cursor(s, t, 0);
}
static void mc_remove_empty(struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    for (size_t i = m->n; i-- > 0;) {
        if (m->n > 1 && !buffer_line_length(&t->doc->buf, line_of(t->doc, m->carets[i].cursor))) mc_remove_at(m, i);
    }
    mc_put_primary(t, m->carets);
}
static void mc_occurrence(int s, struct tab *t, int all)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return;
    if (m->needle && has_sel(t)) {
        size_t a, b; sel_range(t, &a, &b);
        char *selected = buffer_dup(&t->doc->buf, a, b - a);
        if (b - a != m->needle_len || !selected || memcmp(selected, m->needle, m->needle_len)) {
            free(m->needle); m->needle = NULL; m->needle_len = 0; m->skip = 0;
        }
        free(selected);
    }
    if (!m->needle || (!has_sel(t) && m->n == 1)) {
        size_t a, b;
        if (has_sel(t)) sel_range(t, &a, &b); else word_at(t->doc, t->cursor, &a, &b);
        if (a == b) return;
        free(m->needle);
        m->needle = buffer_dup(&t->doc->buf, a, b - a);
        if (!m->needle) return;
        m->needle_len = b - a;
        t->anchor = a; t->cursor = b;
        m->carets[0] = mc_primary(t);
        for (size_t i = 1; i < m->n; i++) if (m->carets[i].anchor == m->carets[i].cursor) {
            size_t ca, cb; word_at(t->doc, m->carets[i].cursor, &ca, &cb);
            m->carets[i].anchor = ca; m->carets[i].cursor = cb;
        }
        if (!all) { set_primary(t); return; }
    }
    const char *text = buffer_text(&t->doc->buf);
    size_t length = doc_len(t->doc), start = t->cursor, search_at = start;
    if (all) {
        m->n = 1; search_at = 0;
    } else if (m->skip) {
        if (m->n > 1) mc_remove_at(m, 0);
        else { m->n = 0; }
        m->skip = 0;
    }
    int wrapped = 0;
    while (search_at <= length && m->needle_len <= length - search_at) {
        const char *found = memmem(text + search_at, length - search_at, m->needle, m->needle_len);
        if (!found) {
            if (!all && !wrapped) { search_at = 0; wrapped = 1; continue; }
            break;
        }
        size_t a = (size_t)(found - text), b = a + m->needle_len;
        if (wrapped && a >= start) break;
        search_at = b;
        size_t existing = m->n;
        for (size_t i = 0; i < m->n; i++) {
            if (min_z(m->carets[i].cursor, m->carets[i].anchor) == a &&
                max_z(m->carets[i].cursor, m->carets[i].anchor) == b) { existing = i; break; }
        }
        if (existing < m->n) {
            if (!all) {
                struct mc_caret old = m->carets[0];
                m->carets[0] = m->carets[existing]; m->carets[existing] = old;
                mc_put_primary(t, m->carets);
                break;
            }
            continue;
        }
        if (!mc_reserve(m, m->n + 1)) break;
        if (all) {
            m->carets[m->n++] = (struct mc_caret){ b, a, -1, -1, -1 };
        } else {
            if (m->n) m->carets[m->n] = m->carets[0];
            m->carets[0] = (struct mc_caret){ b, a, -1, -1, -1 }; m->n++;
            mc_put_primary(t, m->carets);
            break;
        }
    }
    if (!m->n) {
        t->anchor = t->cursor;
        m->n = 1; m->carets[0] = mc_primary(t);
    }
    m->rectangle = 0;
    doc_safe_point(t->doc); set_primary(t); show_cursor(s, t, 0);
}
static void mc_skip(struct tab *t)
{
    struct edit_multicursor *m = mc_state(t);
    if (m) m->skip = 1;
}

/* Operations are ordered from the end of the document towards its start.
 * moved() updates all dormant caret positions; only the currently installed
 * caret is updated by edit.c itself. This is the self-application guard. */
enum mc_operation { MC_TYPE, MC_NEWLINE, MC_BACKSPACE, MC_DELETE, MC_WORD, MC_LINES,
                    MC_INDENT, MC_TAB, MC_COMMENT, MC_PASTE, MC_CUT, MC_MOVE, MC_BRACKET };
struct mc_request { int operation, s, arg, key, mods; const char *text; size_t len; };
static void mc_one(struct tab *t, const struct mc_request *r)
{
    switch (r->operation) {
    case MC_TYPE: type_text(r->s, t, r->text, r->len); break;
    case MC_NEWLINE: newline(r->s, t, r->arg); break;
    case MC_BACKSPACE: key_backspace(r->s, t); break;
    case MC_DELETE: key_delete(r->s, t); break;
    case MC_WORD:
        if (has_sel(t)) remove_selection(t); else delete_word(r->s, t, r->arg);
        break;
    case MC_LINES: delete_lines(r->s, t); break;
    case MC_INDENT: indent_selection(t, r->arg); break;
    case MC_TAB: key_tab(r->s, t); break;
    case MC_COMMENT: (void)comment(r->s, t, r->arg); break;
    case MC_PASTE: {
        char *text = malloc(r->len ? r->len : 1);
        if (text) { memcpy(text, r->text, r->len); paste_text(r->s, t, text, r->len); free(text); }
        break;
    }
    case MC_CUT:
        if (has_sel(t)) remove_selection(t);
        else {
            size_t line = line_of(t->doc, t->cursor), a = line_start(t->doc, line);
            size_t b = line + 1 < t->doc->buf.lines ? line_start(t->doc, line + 1) : line_end(t->doc, line);
            doc_remove(t->doc, a, b - a); t->anchor = t->cursor;
        }
        break;
    case MC_MOVE: view_key(r->s, t, r->key, r->mods); break;
    case MC_BRACKET: to_bracket(r->s, t, r->arg); break;
    }
}
/* A rectangle boundary through a tab selects cells, not an arbitrary byte.
 * Expand only tabs crossed by its edges; preserve all text outside the block.
 * Wide/combined glyphs remain indivisible. Padding is needed only for insert. */
static void mc_materialize(struct tab *t, struct mc_caret *c, int insert)
{
    struct doc *d = t->doc;
    size_t line = line_of(d, c->cursor), end = line_end(d, line);
    int lo = c->column < c->anchor_column ? c->column : c->anchor_column;
    int hi = c->column > c->anchor_column ? c->column : c->anchor_column;
    int x = 0;
    for (size_t p = line_start(d, line); p < end;) {
        int w; uint32_t cp;
        size_t next = cluster(d, p, end, x, &w, &cp);
        if (cp == '\t' && ((lo > x && lo < x + w) || (hi > x && hi < x + w))) {
            char ws[TAB_WIDTH]; memset(ws, ' ', sizeof ws);
            doc_remove(d, p, next - p); doc_insert(d, p, ws, (size_t)w);
            end = line_end(d, line); next = p + (size_t)w;
        }
        x += w; p = next;
    }
    if (insert && x < lo) {
        char *ws = malloc((size_t)(lo - x));
        if (ws) { memset(ws, ' ', (size_t)(lo - x)); doc_insert(d, end, ws, (size_t)(lo - x)); free(ws); }
    }
    t->anchor = mc_at_column(d, line, c->anchor_column);
    t->cursor = mc_at_column(d, line, c->column);
}
struct mc_order { size_t i, at; };
static int mc_order_desc(const void *a, const void *b)
{
    const struct mc_order *x = a, *y = b;
    return x->at < y->at ? 1 : x->at > y->at ? -1 : x->i > y->i ? 1 : x->i < y->i ? -1 : 0;
}
static void mc_history_prune(struct tab *t)
{
    struct edit_multicursor *m = t->mc;
    if (!m) return;
    for (size_t i = 0; i < m->nhistory;) {
        struct mc_history *h = &m->history[i];
        if (h->group >= t->doc->count || t->doc->groups[h->group].items != h->token) {
            mc_history_free(h);
            memmove(m->history + i, m->history + i + 1, (--m->nhistory - i) * sizeof *m->history);
        } else i++;
    }
}
static struct mc_history *mc_history_begin(struct tab *t, size_t group)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m) return NULL;
    for (size_t i = 0; i < m->nhistory;) {
        if (m->history[i].group >= group) {
            mc_history_free(&m->history[i]);
            memmove(m->history + i, m->history + i + 1, (--m->nhistory - i) * sizeof *m->history);
        } else i++;
    }
    struct mc_history *h = realloc(m->history, (m->nhistory + 1) * sizeof *h);
    if (!h) return NULL;
    m->history = h;
    h = &m->history[m->nhistory++];
    *h = (struct mc_history){ .group = group };
    mc_snapshot_take(t, &h->before);
    return h;
}
static struct doc *mc_normal_doc;
static size_t mc_normal_group;
static unsigned mc_normal_depth;
static int mc_normal_snapshots;
static void mc_finish_history(struct doc *d, size_t group)
{
    for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) {
        struct tab *v = &spaces[s].tabs[i]; struct edit_multicursor *m = v->mc;
        if (v->doc != d || !m || !m->nhistory) continue;
        struct mc_history *h = &m->history[m->nhistory - 1];
        if (h->group != group) continue;
        if (d->done > group) { h->token = d->groups[group].items; mc_snapshot_take(v, &h->after); }
        else { mc_history_free(h); m->nhistory--; }
    }
}
/* Single-caret edits in another split must snapshot dormant multicarets too.
 * Preserve ordinary single-caret typing merge behavior when no view has extras. */
static void mc_doc_begin(struct doc *d, size_t c, size_t a)
{
    if (d == mc_transaction_doc) return;
    if (d == mc_normal_doc) { mc_normal_depth++; return; }
    mc_normal_doc = d; mc_normal_depth = 1; mc_normal_group = d->done; mc_normal_snapshots = 0;
    for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) {
        struct tab *v = &spaces[s].tabs[i];
        if (v->doc == d && mc_active(v)) mc_normal_snapshots = 1;
    }
    if (mc_normal_snapshots) {
        doc_safe_point(d);
        for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) {
            struct tab *v = &spaces[s].tabs[i];
            if (v->doc == d) mc_history_begin(v, d->done);
        }
    }
    (doc_begin)(d, c, a);
}
static void mc_doc_end(struct doc *d, size_t c, size_t a)
{
    if (d == mc_transaction_doc) return;
    if (d == mc_normal_doc && mc_normal_depth > 1) { mc_normal_depth--; return; }
    (doc_end)(d, c, a);
    if (d == mc_normal_doc) {
        if (mc_normal_snapshots) { doc_safe_point(d); mc_finish_history(d, mc_normal_group); }
        else for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) {
            struct tab *v = &spaces[s].tabs[i];
            if (v->doc == d) mc_history_prune(v);
        }
        mc_normal_doc = NULL; mc_normal_depth = 0;
    }
}
static void mc_prepare_removals(struct tab *t, const struct mc_request *r)
{
    struct edit_multicursor *m = mc_state(t);
    if (!m || m->rectangle) return;
    for (size_t i = 0; i < m->n; i++) {
        struct mc_caret *c = &m->carets[i];
        if (c->cursor != c->anchor) continue;
        size_t at = c->cursor, to = at;
        if (r->operation == MC_WORD) to = r->arg ? word_next(t->doc, at) : word_prev(t->doc, at);
        else if (r->operation == MC_DELETE) to = buffer_next_cluster(&t->doc->buf, at);
        else if (r->operation == MC_BACKSPACE) {
            size_t line = line_of(t->doc, at), ls = line_start(t->doc, line);
            long first = first_char(t->doc, line);
            if (at > ls && (first < 0 || at <= ls + (size_t)first)) continue;
            to = buffer_prev_char(&t->doc->buf, at);
        } else if (r->operation == MC_CUT) {
            size_t line = line_of(t->doc, at);
            c->anchor = line_start(t->doc, line);
            c->cursor = line + 1 < t->doc->buf.lines ? line_start(t->doc, line + 1) : line_end(t->doc, line);
            continue;
        } else continue;
        c->anchor = at; c->cursor = to;
    }
    mc_put_primary(t, m->carets);
    mc_normalize(t);
}
static int mc_run(struct tab *t, const struct mc_request *r)
{
    if (!mc_active(t)) return 0;
    int editing = r->operation != MC_MOVE && r->operation != MC_BRACKET;
    if (editing && !writable(t->doc)) return 1;
    if ((r->operation == MC_TYPE || r->operation == MC_PASTE) && !r->len) return 1;
    struct edit_multicursor *m = mc_state(t);
    if (!m) return 1;
    struct mc_order *order = malloc(m->n * sizeof *order);
    if (!order) return 1;
    for (size_t i = 0; i < m->n; i++) order[i] = (struct mc_order){ i, max_z(m->carets[i].cursor, m->carets[i].anchor) };
    qsort(order, m->n, sizeof *order, mc_order_desc);
    size_t clipboard_rows = 1;
    if (r->operation == MC_PASTE) for (size_t p = 0; p < r->len; p++) clipboard_rows += r->text[p] == '\n';
    size_t *paste_row = NULL;
    if (r->operation == MC_PASTE && clipboard_rows == m->n) {
        paste_row = malloc(m->n * sizeof *paste_row);
        if (!paste_row) { free(order); return 1; }
        for (size_t k = 0; k < m->n; k++) paste_row[order[k].i] = m->n - k - 1;
    }
    struct doc *d = t->doc;
    size_t group = d->done, original_cursor = t->cursor, original_anchor = t->anchor;
    if (editing) {
        /* Snapshot every view of this document, not just the initiating view. */
        for (int s = 0; s < nspaces; s++) for (int i = 0; i < spaces[s].ntabs; i++) {
            struct tab *v = &spaces[s].tabs[i];
            if (v->doc == d) mc_history_begin(v, group);
        }
        mc_prepare_removals(t, r);
        for (size_t i = 0; i < m->n; i++) order[i] = (struct mc_order){ i, max_z(m->carets[i].cursor, m->carets[i].anchor) };
        qsort(order, m->n, sizeof *order, mc_order_desc);
        doc_safe_point(d);
        (doc_begin)(d, original_cursor, original_anchor);
        mc_transaction_doc = d;
    }
    m->busy = 1;
    /* Indentation/comment/line removal are line operations: process each
     * touched line just once even when several carets share that line. */
    unsigned char *lines = NULL;
    if (r->operation == MC_INDENT || r->operation == MC_COMMENT || r->operation == MC_LINES)
        lines = calloc(d->buf.lines, 1);
    unsigned char *indent_lines = NULL;
    if (r->operation == MC_TAB || r->operation == MC_BACKSPACE) indent_lines = calloc(d->buf.lines, 1);
    if (lines) {
        size_t nlines = d->buf.lines;
        for (size_t i = 0; i < m->n; i++) {
            size_t a = min_z(m->carets[i].cursor, m->carets[i].anchor);
            size_t b = max_z(m->carets[i].cursor, m->carets[i].anchor);
            size_t first = line_of(d, a), last = line_of(d, b);
            if (b > a && last > first && b == line_start(d, last)) last--;
            for (size_t l = first; l <= last; l++) lines[l] = 1;
        }
        m->current = SIZE_MAX; /* temporary line selections are not real carets */
        for (size_t end = nlines; end;) {
            if (!lines[--end]) continue;
            size_t first = end;
            while (first && lines[first - 1]) first--;
            t->anchor = line_start(d, first);
            t->cursor = line_end(d, end);
            if (r->operation == MC_INDENT) change_indent(t, t->anchor, t->cursor, r->arg);
            else mc_one(t, r);
            end = first;
        }
    }
    for (size_t k = 0; !lines && k < m->n; k++) {
        size_t i = order[k].i;
        m->current = i;
        mc_put_primary(t, &m->carets[i]);
        if (indent_lines && !has_sel(t)) {
            size_t line = line_of(d, t->cursor), ls = line_start(d, line);
            long first = first_char(d, line);
            int leading = first < 0 || t->cursor <= ls + (size_t)first;
            if (leading && (r->operation == MC_TAB || t->cursor > ls)) {
                if (indent_lines[line]) continue;
                indent_lines[line] = 1;
            }
        }
        if (m->rectangle && editing && r->operation != MC_INDENT && r->operation != MC_COMMENT && r->operation != MC_LINES) {
            int insert = r->operation == MC_TYPE || r->operation == MC_TAB || r->operation == MC_NEWLINE || r->operation == MC_PASTE;
            mc_materialize(t, &m->carets[i], insert);
            /* A nonempty rectangle can select only virtual cells on a short
             * row. Deleting/cutting that row must not Smart-Cut its real line
             * or join the following line through its newline. */
            if (!insert && m->acol != m->ccol && !has_sel(t)) {
                m->carets[i] = mc_primary(t);
                continue;
            }
        }
        if (r->operation == MC_PASTE && paste_row) {
            /* Kate multiPaste: one clipboard row per caret, in document
             * order, when row and caret counts match; full text otherwise. */
            size_t row = paste_row[i], from = 0, to;
            for (size_t l = 0; l < row && from < r->len; l++) {
                while (from < r->len && r->text[from] != '\n') from++;
                if (from < r->len) from++;
            }
            to = from; while (to < r->len && r->text[to] != '\n') to++;
            struct mc_request one = *r; one.text += from; one.len = to - from;
            if (has_sel(t)) remove_selection(t);
            mc_one(t, &one);
        } else mc_one(t, r);
        int col = m->carets[i].column, anchor_col = m->carets[i].anchor_column;
        m->carets[i] = mc_primary(t);
        if (!editing) { m->carets[i].column = col; m->carets[i].anchor_column = anchor_col; }
    }
    free(paste_row); free(indent_lines); free(lines); free(order);
    m->busy = 0;
    m->rectangle = 0;
    mc_put_primary(t, m->carets);
    mc_normalize(t);
    if (editing) {
        mc_transaction_doc = NULL;
        (doc_end)(d, t->cursor, t->anchor);
        doc_safe_point(d);
        mc_finish_history(d, group);
        edited(t);
    }
    mc_normalize(t);
    show_cursor(r->s, t, 0);
    if (m->block) {
        m->aline = m->cline = line_of(d, t->cursor);
        m->acol = m->ccol = mc_column(d, t->cursor);
    }
    return 1;
}
static int mc_type_text(int s, struct tab *t, const char *text, size_t len)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_TYPE, .s = s, .text = text, .len = len });
}
static int mc_newline(int s, struct tab *t, int how)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_NEWLINE, .s = s, .arg = how });
}
static int mc_backspace(int s, struct tab *t)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_BACKSPACE, .s = s });
}
static int mc_delete(int s, struct tab *t)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_DELETE, .s = s });
}
static int mc_delete_word(int s, struct tab *t, int right)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_WORD, .s = s, .arg = right });
}
static int mc_delete_lines(int s, struct tab *t)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_LINES, .s = s });
}
static int mc_indent(struct tab *t, int change)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_INDENT, .s = active, .arg = change });
}
static int mc_tab(int s, struct tab *t)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_TAB, .s = s });
}
static int mc_comment(int s, struct tab *t, int how)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_COMMENT, .s = s, .arg = how });
}
static int mc_to_bracket(int s, struct tab *t, int sel)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_BRACKET, .s = s, .arg = sel });
}
static int mc_paste(int s, struct tab *t, char *text, size_t n)
{
    return mc_run(t, &(struct mc_request){ .operation = MC_PASTE, .s = s, .text = text, .len = n });
}
static int mc_undo_redo(int s, struct tab *t, int redo)
{
    struct doc *d = t->doc;
    size_t group = redo ? d->done : d->done ? d->done - 1 : 0;
    if (group >= d->count) return 1;
    const struct undo_item *token = d->groups[group].items;
    size_t cursor, anchor;
    if (!(redo ? doc_redo : doc_undo)(d, &cursor, &anchor)) return 1;
    t->cursor = cursor; t->anchor = anchor;
    for (int sp = 0; sp < nspaces; sp++) for (int i = 0; i < spaces[sp].ntabs; i++) {
        struct tab *v = &spaces[sp].tabs[i];
        if (v->doc != d || !v->mc) continue;
        for (size_t j = 0; j < v->mc->nhistory; j++) {
            struct mc_history *h = &v->mc->history[j];
            if (h->group == group && h->token == token) {
                mc_snapshot_restore(v, redo ? &h->after : &h->before); break;
            }
        }
    }
    edited(t); show_cursor(s, t, 0);
    return 1;
}

/* Clipboard text in document order. Empty selections use Smart Copy Cut's
 * whole-line behavior, once per line. Block copy has fixed-width rows and
 * includes selected portions of tabs and virtual spaces past short lines. */
struct mc_string { char *s; size_t n, cap; };
static int mc_append(struct mc_string *s, const char *text, size_t n)
{
    if (n > SIZE_MAX - s->n - 1) return 0;
    if (s->n + n + 1 > s->cap) {
        size_t cap = s->n + n + 1;
        char *p = realloc(s->s, cap);
        if (!p) return 0;
        s->s = p; s->cap = cap;
    }
    if (n) memcpy(s->s + s->n, text, n);
    s->n += n; s->s[s->n] = '\0'; return 1;
}
static int mc_spaces(struct mc_string *s, int n)
{
    for (int i = 0; i < n; i++) if (!mc_append(s, " ", 1)) return 0;
    return 1;
}
static int mc_block_copy_row(struct mc_string *out, const struct tab *t, size_t line, int lo, int hi)
{
    const struct doc *d = t->doc;
    int x = 0; size_t end = line_end(d, line);
    for (size_t p = line_start(d, line); p < end && x < hi;) {
        int w; uint32_t cp;
        size_t next = cluster(d, p, end, x, &w, &cp);
        int from = x > lo ? x : lo, to = x + w < hi ? x + w : hi;
        if (from < to) {
            if (cp == '\t') { if (!mc_spaces(out, to - from)) return 0; }
            else {
                char *text = buffer_dup(&d->buf, p, next - p);
                int ok = text && mc_append(out, text, next - p); free(text);
                if (!ok) return 0;
            }
        }
        x += w; p = next;
    }
    return x >= hi || mc_spaces(out, hi - (x > lo ? x : lo));
}
static int mc_copy(struct tab *t, int cut, int primary)
{
    if (!mc_active(t)) return 0;
    struct edit_multicursor *m = mc_state(t);
    if (!m) return 1;
    struct mc_string out = {0};
    if (m->rectangle) {
        int lo = m->acol < m->ccol ? m->acol : m->ccol, hi = m->acol > m->ccol ? m->acol : m->ccol;
        if (lo == hi) return 1;
        size_t first = min_z(m->aline, m->cline), last = max_z(m->aline, m->cline);
        for (size_t line = first; line <= last; line++) {
            if (line > first && !mc_append(&out, "\n", 1)) goto done;
            if (!mc_block_copy_row(&out, t, line, lo, hi)) goto done;
        }
    } else {
        struct mc_order *order = malloc(m->n * sizeof *order);
        if (!order) return 1;
        for (size_t i = 0; i < m->n; i++) order[i] = (struct mc_order){ i, min_z(m->carets[i].cursor, m->carets[i].anchor) };
        qsort(order, m->n, sizeof *order, mc_order_desc);
        size_t previous_line = SIZE_MAX;
        for (size_t k = m->n; k-- > 0;) {
            struct mc_caret *c = &m->carets[order[k].i];
            size_t a = min_z(c->cursor, c->anchor), b = max_z(c->cursor, c->anchor);
            int linecopy = a == b;
            if (linecopy) {
                if (primary) continue;
                size_t line = line_of(t->doc, a);
                if (line == previous_line) continue;
                previous_line = line; a = line_start(t->doc, line);
                b = line + 1 < t->doc->buf.lines ? line_start(t->doc, line + 1) : line_end(t->doc, line);
            }
            char *text = buffer_dup(&t->doc->buf, a, b - a);
            if (!text) { free(order); goto done; }
            if (out.n && out.s[out.n - 1] != '\n') mc_append(&out, "\n", 1);
            int ok = mc_append(&out, text, b - a);
            free(text);
            if (!ok) { free(order); goto done; }
            if (linecopy && (!out.n || out.s[out.n - 1] != '\n')) mc_append(&out, "\n", 1);
        }
        free(order);
    }
    if (out.s) clipboard_set(primary, out.s, out.n);
    if (cut && !primary) mc_run(t, &(struct mc_request){ .operation = MC_CUT, .s = active });
done:
    free(out.s);
    return 1;
}

static int mc_action(int s, struct tab *t, int key, int mods)
{
    if (t->mc && t->mc->busy) return 0;
    int ctrl = !!(mods & KEYS_CTRL), alt = !!(mods & KEYS_ALT), shift = !!(mods & KEYS_SHIFT);
    if (ctrl && alt && !shift && (key == K_UP || key == K_DOWN)) {
        mc_add_vertical(s, t, key == K_UP ? -1 : 1); return 1;
    }
    if (alt && shift && !ctrl && key == 'i') { mc_line_ends(s, t); return 1; }
    if (alt && key == 'j' && ((!ctrl && !shift) || (ctrl && shift))) {
        mc_occurrence(s, t, ctrl && shift); return 1;
    }
    if (alt && !ctrl && !shift && key == 'k') { mc_skip(t); return 1; }
    if (ctrl && shift && !alt && key == 'b') { mc_toggle_block(t); return 1; }
    if (ctrl && !alt && key == 'a') {
        if (shift) mc_deselect(t); else mc_select_all(t);
        return 1;
    }
    return 0;
}
static int mc_view_key(int s, struct tab *t, int key, int mods)
{
    if (t->mc && t->mc->busy) return 0;
    int ctrl = !!(mods & KEYS_CTRL), alt = !!(mods & KEYS_ALT), shift = !!(mods & KEYS_SHIFT);
    if (key == 27 && !ctrl && !alt && (mc_count(t) > 1 || (t->mc && t->mc->rectangle))) {
        mc_clear(t); t->anchor = t->cursor; return 1;
    }
    int navigation = key == K_LEFT || key == K_RIGHT || key == K_UP || key == K_DOWN || key == K_BOL ||
                     key == K_EOL || key == K_PGUP || key == K_PGDN;
    if (!navigation) return 0;
    if (mc_block(t) && shift && !alt) {
        struct edit_multicursor *m = mc_state(t);
        if (!m->rectangle) {
            m->aline = m->cline = line_of(t->doc, t->cursor);
            m->acol = m->ccol = mc_column(t->doc, t->cursor);
        }
        if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN) {
            int y, h; text_rows(s, &y, &h);
            long delta = key == K_UP || key == K_PGUP ? -1 : 1;
            if (key == K_PGUP || key == K_PGDN) delta *= h > 1 ? h - 1 : 1;
            long line = (long)m->cline + delta;
            m->cline = line < 0 ? 0 : (size_t)line >= t->doc->buf.lines ? t->doc->buf.lines - 1 : (size_t)line;
        } else if (key == K_BOL || key == K_EOL) {
            if (ctrl) m->cline = key == K_BOL ? 0 : t->doc->buf.lines - 1;
            m->ccol = key == K_BOL ? 0 : mc_column(t->doc, line_end(t->doc, m->cline));
        } else if (ctrl) {
            size_t at = mc_at_column(t->doc, m->cline, m->ccol);
            at = key == K_RIGHT ? word_next(t->doc, at) : word_prev(t->doc, at);
            m->cline = line_of(t->doc, at); m->ccol = mc_column(t->doc, at);
        } else if (key == K_LEFT) { if (m->ccol > 0) m->ccol--; }
        else m->ccol++;
        mc_rectangle(t); doc_safe_point(t->doc); show_cursor(s, t, 0);
        primary_pending = 1; primary_since = now(); return 1;
    }
    /* Ctrl+Up/Down scroll the shared viewport once, not once per caret. */
    if (ctrl && !shift && (key == K_UP || key == K_DOWN)) return 0;
    if (t->mc) for (size_t i = 0; i < t->mc->n; i++) {
        t->mc->carets[i].column = t->mc->carets[i].anchor_column = -1;
    }
    return mc_run(t, &(struct mc_request){ .operation = MC_MOVE, .s = s, .key = key, .mods = mods });
}
static int mc_pointer_column(int s, const struct pointer_event *e, size_t at)
{
    struct tab *t = tab_of(s);
    int x, w, bw, bh, top, cw, ch;
    space_cols(s, &x, &w); screen_cells(&bw, &bh, &top);
    int own = content_metrics(s, &cw, &ch, &top), col;
    if (!own) { cw = bw; ch = bh; }
    (void)ch;
    col = cw > 0 ? (e->x - x * bw - gutter(t) * cw + cw / 2) / cw : e->col - x - gutter(t);
    if (col < 0) col = 0;
    int row, px;
    place_of(t, text_width(s, t), at, &row, &px);
    if (!t->wrap) return col + t->left;
    /* A wrapped row's hit position supplies the logical prefix; pointer
     * distance beyond EOL supplies the block's virtual trailing columns. */
    return mc_column(t->doc, at) + (col > px ? col - px : 0);
}
static int mc_click(int s, struct tab *t, const struct pointer_event *e, size_t at)
{
    int mods = keys_modifiers();
    if ((mods & KEYS_ALT) && !(mods & KEYS_CTRL)) {
        struct edit_multicursor *m = mc_state(t);
        if (!m) return 1;
        active = s; bar.focus = FOCUS_VIEW; tree_focus = 0; drag.mode = DRAG_NONE;
        size_t found = m->n;
        for (size_t i = 0; i < m->n; i++) {
            size_t a = min_z(m->carets[i].cursor, m->carets[i].anchor), b = max_z(m->carets[i].cursor, m->carets[i].anchor);
            if (m->carets[i].cursor == at || (at >= a && at < b)) { found = i; break; }
        }
        if (found < m->n) {
            if (m->n > 1) { mc_remove_at(m, found); mc_put_primary(t, m->carets); }
        } else {
            mc_add(t, (struct mc_caret){ at, at, -1, -1, -1 }, 1);
            drag = (typeof(drag)){ DRAG_CHARS, s, at, at, e->row };
        }
        m->rectangle = 0; doc_safe_point(t->doc); show_cursor(s, t, 0); return 1;
    }
    if (mc_block(t)) {
        struct edit_multicursor *m = mc_state(t);
        active = s; bar.focus = FOCUS_VIEW; tree_focus = 0;
        if (!(mods & KEYS_SHIFT)) {
            m->aline = line_of(t->doc, at); m->acol = mc_pointer_column(s, e, at);
        } else if (!m->rectangle) {
            m->aline = line_of(t->doc, t->anchor); m->acol = mc_column(t->doc, t->anchor);
        }
        m->cline = line_of(t->doc, at); m->ccol = mc_pointer_column(s, e, at);
        mc_rectangle(t);
        drag = (typeof(drag)){ DRAG_CHARS, s, t->anchor, t->anchor, e->row };
        doc_safe_point(t->doc); return 1;
    }
    mc_clear(t);
    return 0;
}
static int mc_drag(struct tab *t, size_t at)
{
    if (!mc_block(t)) return 0;
    struct edit_multicursor *m = mc_state(t);
    m->cline = line_of(t->doc, at);
    m->ccol = mc_pointer_column(drag.space, pointer_last(), at);
    mc_rectangle(t); t->goal = -1; return 1;
}
static int mc_selected(const struct tab *t, size_t at)
{
    if (!t->mc) return at >= min_z(t->cursor, t->anchor) && at < max_z(t->cursor, t->anchor);
    const struct edit_multicursor *m = t->mc;
    if (m->rectangle) {
        size_t line = line_of(t->doc, at);
        if (line < min_z(m->aline, m->cline) || line > max_z(m->aline, m->cline) || at == line_end(t->doc, line)) return 0;
        int x = mc_column(t->doc, at), w; uint32_t cp;
        cluster(t->doc, at, line_end(t->doc, line), x, &w, &cp);
        int lo = m->acol < m->ccol ? m->acol : m->ccol, hi = m->acol > m->ccol ? m->acol : m->ccol;
        /* Partial tabs are painted cell-by-cell in mc_draw_carets. */
        return cp == '\t' ? x >= lo && x + w <= hi : x < hi && x + w > lo;
    }
    if (at >= min_z(t->cursor, t->anchor) && at < max_z(t->cursor, t->anchor)) return 1;
    for (size_t i = 1; i < m->n; i++) {
        const struct mc_caret *c = &m->carets[i];
        if (at >= min_z(c->cursor, c->anchor) && at < max_z(c->cursor, c->anchor)) return 1;
    }
    return 0;
}
static void mc_draw_carets(int s, struct tab *t, int x0, int y0, int width, int height, int focus)
{
    if (!t->mc) return;
    struct edit_multicursor *m = t->mc;
    for (size_t i = m->rectangle ? 0 : 1; i < m->n; i++) {
        struct mc_caret c = m->carets[i];
        if (i == 0) c.cursor = t->cursor;
        size_t target = line_of(t->doc, c.cursor), line = line_of(t->doc, t->top);
        int row = t->sub, want_row, x;
        place_of(t, width, c.cursor, &want_row, &x);
        if (m->rectangle && c.column > mc_column(t->doc, c.cursor)) x += c.column - mc_column(t->doc, c.cursor);
        x -= t->left;
        for (int y = 0; y < height; y++) {
            if (line == target && row == want_row) {
                if (m->rectangle) {
                    int lo = m->acol < m->ccol ? m->acol : m->ccol;
                    int hi = m->acol > m->ccol ? m->acol : m->ccol;
                    int end_col = mc_column(t->doc, line_end(t->doc, line));
                    /* Correct the background of partial tabs without changing
                     * document bytes merely because a block is selected. */
                    int logical = 0;
                    size_t le = line_end(t->doc, line);
                    for (size_t p = line_start(t->doc, line); p < le;) {
                        int pw; uint32_t cp;
                        size_t next = cluster(t->doc, p, le, logical, &pw, &cp);
                        if (cp == '\t' && logical < hi && logical + pw > lo &&
                            !(logical >= lo && logical + pw <= hi)) {
                            int pr, px; place_of(t, width, p, &pr, &px);
                            if (pr == row) {
                                int a = (lo > logical ? lo : logical) - logical;
                                int b = (hi < logical + pw ? hi : logical + pw) - logical;
                                a += px - t->left; b += px - t->left;
                                a = a < 0 ? 0 : a; b = b > width ? width : b;
                                if (b > a) content_fill(x0 + a, y0 + y, b - a, 1, C_SELECT);
                            }
                        }
                        logical += pw; p = next;
                    }
                    int from = lo > end_col ? lo : end_col;
                    if (row == line_rows(t, width, line) - 1 && hi > from) {
                        int caret_col = mc_column(t->doc, c.cursor);
                        if (c.column > caret_col) caret_col = c.column;
                        int base = caret_col - (x + t->left);
                        int a = from - base - t->left, b = hi - base - t->left;
                        a = a < 0 ? 0 : a; b = b > width ? width : b;
                        if (b > a) content_fill(x0 + a, y0 + y, b - a, 1, C_SELECT);
                    }
                }
                if (focus && x >= 0 && x < width && (i || m->rectangle)) {
                    char glyph[8] = " "; size_t n;
                    uint32_t cp = buffer_char(&t->doc->buf, c.cursor, &n);
                    if (n && cp >= 0x20 && cp != 0x7f && c.cursor < line_end(t->doc, line)) glyph[text_put(glyph, cp)] = '\0';
                    content_caret(x0 + x, y0 + y, t->doc->overwrite, glyph, mc_selected(t, c.cursor) ? C_SELECT : C_BG);
                }
                break;
            }
            size_t old = line; int oldrow = row;
            move_rows(t, width, &line, &row, 1);
            if (line == old && row == oldrow) break;
        }
    }
    (void)s;
}
