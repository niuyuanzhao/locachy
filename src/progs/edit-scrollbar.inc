/*
 * Each view's vertical scrollbar, its marks, its mini-map and its text
 * preview; the marks they show (bookmarks, Find All's and Replace All's
 * matches) and the line modification system with its markers in the border
 * and Go to Previous/Next Modified Line; Remove Spaces on save. As
 * KTextEditor has them:
 * KateScrollBar (src/view/kateviewhelpers.cpp), the document's marks,
 * typeChars and removeTrailingSpacesAndAddNewLineAtEof
 * (src/document/katedocument.cpp), KateBookmarks, the search bar's
 * SearchMatch marks (src/search/katesearchbar.cpp), with the defaults of
 * src/utils/kateconfig.cpp. Included in edit.c.
 *
 * The cells: the scrollbar is one column at the right of the text; the
 * mini-map, which replaces it, is Scroll Bar Mini Map Width (60 pixels) of
 * columns, its first (s_pixelMargin) for the modified and saved lines, the
 * others for up to s_lineWidth (100) characters of each line, the lines at
 * most two pixel rows each (docHeight), every cell the colour most of its
 * characters have.
 */
#define MINIMAP_PX     60   /* Scroll Bar Mini Map Width */
#define MINIMAP_CHARS  100  /* s_lineWidth */
#define MINIMAP_SAMPLE 4    /* lines of a cell row read for its colours */
#define REMOVE_SPACES  1    /* Remove Spaces: 0 never, 1 on modified lines (the default), 2 everywhere */
#define C_GROOVE       0x31363B
#define C_SLIDER       0x5A6068
enum { LINE_MODIFIED = 1, LINE_SAVED = 2 };

/* The mini-map's colours, cached for a view until its document or size changes. */
static struct minimap_cache {
    const struct doc *doc;
    unsigned long version;
    size_t total;
    int rows, cols, h, folded;
    uint32_t *cells;         /* rows x cols, the margin column's included */
    unsigned char *marks;    /* rows: LINE_* of its lines */
} minimap_cache[MAX_SPACES];

/* Anchors: line starts in order, each with flags. */
static size_t anchors_find(const struct anchors *a, size_t at)
{
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->at[mid] < at) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int anchors_reserve(struct anchors *a, size_t n)
{
    if (n <= a->size) return 0;
    size_t size = a->size ? a->size : 16;
    while (size < n) size *= 2;
    size_t *at = realloc(a->at, size * sizeof *at);
    if (!at) return -1;
    a->at = at;
    unsigned char *flag = realloc(a->flag, size);
    if (!flag) return -1;
    a->flag = flag;
    a->size = size;
    return 0;
}

static void anchors_clear(struct anchors *a)
{
    a->n = 0;
}

static void anchors_free(struct anchors *a)
{
    free(a->at);
    free(a->flag);
    memset(a, 0, sizeof *a);
}

static int anchors_copy(struct anchors *to, const struct anchors *from)
{
    if (anchors_reserve(to, from->n) < 0) return -1;
    if (from->n) {
        memcpy(to->at, from->at, from->n * sizeof *to->at);
        memcpy(to->flag, from->flag, from->n);
    }
    to->n = from->n;
    return 0;
}

/* Line starts [from, to] given flag, in one pass over those already there. */
static void anchors_mark(struct doc *d, struct anchors *a, size_t first, size_t last, unsigned char flag)
{
    size_t count = last - first + 1, lo = anchors_find(a, line_start(d, first));
    size_t hi = anchors_find(a, line_start(d, last) + 1), had = hi - lo;
    if (anchors_reserve(a, a->n + count) < 0) return;
    size_t *old_at = had ? malloc(had * sizeof *old_at) : NULL;
    unsigned char *old_flag = had ? malloc(had) : NULL;
    if (had && (!old_at || !old_flag)) {
        free(old_at);
        free(old_flag);
        return;
    }
    if (had) {
        memcpy(old_at, a->at + lo, had * sizeof *old_at);
        memcpy(old_flag, a->flag + lo, had);
    }
    memmove(a->at + lo + count, a->at + hi, (a->n - hi) * sizeof *a->at);
    memmove(a->flag + lo + count, a->flag + hi, a->n - hi);
    a->n += count - had;
    size_t k = 0;
    for (size_t l = 0; l < count; l++) {
        size_t at = line_start(d, first + l);
        unsigned char f = flag;
        while (k < had && old_at[k] <= at) {
            if (old_at[k] == at) f |= old_flag[k];
            k++;
        }
        a->at[lo + l] = at;
        a->flag[lo + l] = f;
    }
    free(old_at);
    free(old_flag);
}

/* After an edit of [at, at + removed) into inserted bytes: anchors move as
 * marks move with their lines (one at the start of a line text is put at
 * goes on with that line; one in removed text to the line left), those in
 * the changed text made line starts again, two on one line made one. */
static void anchors_moved(struct doc *d, struct anchors *a, size_t at, size_t removed, size_t inserted)
{
    size_t i = anchors_find(a, at), w = i;
    for (size_t k = i; k < a->n; k++) {
        size_t p = moved_pos(a->at[k], at, removed, inserted, 1);
        if (p >= at && p <= at + inserted) {
            p = line_start(d, line_of(d, p));
        }
        if (w > 0 && a->at[w - 1] >= p) {
            a->flag[w - 1] |= a->flag[k]; /* joined: the line keeps both */
            continue;
        }
        a->at[w] = p;
        a->flag[w] = a->flag[k];
        w++;
    }
    a->n = w;
}

static int anchors_has(const struct anchors *a, size_t at, unsigned char flag)
{
    size_t i = anchors_find(a, at);
    return i < a->n && a->at[i] == at && (a->flag[i] & flag);
}

/* The flags a saved or loaded document had, when undo or redo reaches it
 * again (KateUndo restores the lines' flags as they were). */
static void lines_sync(struct doc *d)
{
    struct docui *u = ui_of(d);
    if (u->lines_restore) {
        anchors_copy(&u->lines, &u->saved_lines);
        u->lines_restore = 0;
        u->version++; /* the mini-map's markers changed */
    }
}

static void marks_moved(struct doc *d, size_t at, size_t removed, size_t inserted)
{
    struct docui *u = ui_of(d);
    u->version++;
    if (!d->open && d->count == 0) {
        /* The text set or read anew (load, reload): no marks, nothing modified. */
        anchors_clear(&u->bookmarks);
        anchors_clear(&u->lines);
        anchors_clear(&u->saved_lines);
        u->lines_restore = 0;
        u->autobrace = 0;
        return;
    }
    if (u->lines_restore && (d->open || d->done != u->restore_done)) {
        lines_sync(d); /* the undo that reached the saved text is over */
    }
    anchors_moved(d, &u->bookmarks, at, removed, inserted);
    anchors_moved(d, &u->lines, at, removed, inserted);
    if (u->autobrace) {
        u->ab_start = moved_pos(u->ab_start, at, removed, inserted, 1);
        u->ab_end = moved_pos(u->ab_end, at, removed, inserted, 0);
        if (u->ab_end <= u->ab_start) u->autobrace = 0;
    }
    /* The lines the edit touched are modified (editInsertText, editWrapLine). */
    size_t end = at + inserted <= doc_len(d) ? at + inserted : doc_len(d);
    anchors_mark(d, &u->lines, line_of(d, at), line_of(d, end), LINE_MODIFIED);
    if (!d->open && !doc_modified(d)) {
        u->lines_restore = 1; /* undo or redo back to the saved text */
        u->restore_done = d->done;
    } else if (!d->open) {
        u->lines_restore = 0;
    }
    if (d->open) {
        static long edit_clock;
        u->edited = ++edit_clock; /* the Documents list's edit history */
    }
}

static void marks_free(struct doc *d)
{
    struct docui *u = ui_of(d);
    for (int s = 0; s < MAX_SPACES; s++) {
        if (minimap_cache[s].doc == d) minimap_cache[s].doc = NULL;
    }
    anchors_free(&u->bookmarks);
    anchors_free(&u->lines);
    anchors_free(&u->saved_lines);
}

static void lines_loaded(struct doc *d)
{
    struct docui *u = ui_of(d);
    anchors_clear(&u->lines);
    anchors_clear(&u->saved_lines);
    u->lines_restore = 0;
}

/* Saved: the modified lines become saved ones (markAsSavedOnDisk). */
static void lines_saved(struct doc *d)
{
    struct docui *u = ui_of(d);
    lines_sync(d);
    for (size_t i = 0; i < u->lines.n; i++) {
        if (u->lines.flag[i] & LINE_MODIFIED) u->lines.flag[i] = LINE_SAVED;
    }
    anchors_copy(&u->saved_lines, &u->lines);
    u->version++;
}

/* Remove Spaces (on the lines modified or saved since the file was read, as
 * the line modification system marks them), then Newline at End of File:
 * one edit, before the document is written. */
static void save_cleanups(struct doc *d)
{
    struct docui *u = ui_of(d);
    struct tab *t = cur_tab();
    size_t c = t && t->doc == d ? t->cursor : doc_len(d), a = t && t->doc == d ? t->anchor : c;
    lines_sync(d);
    size_t n = 0, *lines = NULL;
    if (REMOVE_SPACES == 2) {
        n = d->buf.lines;
    } else if (REMOVE_SPACES == 1) {
        n = u->lines.n;
    }
    if (n && (lines = malloc(n * sizeof *lines))) {
        for (size_t i = 0; i < n; i++) {
            lines[i] = REMOVE_SPACES == 2 ? i : line_of(d, u->lines.at[i]);
        }
    } else {
        n = 0;
    }
    doc_begin(d, c, a);
    for (size_t i = n; i > 0; i--) {
        size_t line = lines[i - 1], ls = line_start(d, line), le = line_end(d, line), p = le;
        while (p > ls && (byte_at(d, p - 1) == ' ' || byte_at(d, p - 1) == '\t' || byte_at(d, p - 1) == '\v' ||
                          byte_at(d, p - 1) == '\f')) {
            p--;
        }
        if (p < le) doc_remove(d, p, le - p);
    }
    free(lines);
    size_t len = doc_len(d);
    if (len > 0 && byte_at(d, len - 1) != '\n') {
        doc_insert(d, len, "\n", 1);
    }
    if (t && t->doc == d) {
        c = t->cursor;
        a = t->anchor;
    }
    doc_end(d, c, a);
}

/* Bookmarks (KateBookmarks): Set Bookmark, Clear All Bookmarks, Next and
 * Previous Bookmark, cycling (Cycle Through Bookmarks is on). */
static int bookmarked(struct doc *d, size_t line)
{
    struct docui *u = ui_of(d);
    return u->bookmarks.n && anchors_has(&u->bookmarks, line_start(d, line), 1);
}

static void bookmark_set(struct doc *d, size_t line, int on)
{
    struct anchors *a = &ui_of(d)->bookmarks;
    size_t at = line_start(d, line), i = anchors_find(a, at);
    int there = i < a->n && a->at[i] == at;
    if (on && !there) {
        anchors_mark(d, a, line, line, 1);
    } else if (!on && there) {
        memmove(a->at + i, a->at + i + 1, (a->n - i - 1) * sizeof *a->at);
        memmove(a->flag + i, a->flag + i + 1, a->n - i - 1);
        a->n--;
    }
}

static void bookmark_toggle(struct tab *t)
{
    size_t line = line_of(t->doc, t->cursor);
    bookmark_set(t->doc, line, !bookmarked(t->doc, line));
}

static void bookmarks_clear(struct doc *d)
{
    anchors_clear(&ui_of(d)->bookmarks);
}

static void bookmark_go(int s, struct tab *t, size_t line)
{
    move_to(s, t, line_start(t->doc, line), 0, 0);
    show_cursor(s, t, 0);
}

static void bookmark_step(int s, struct tab *t, int dir)
{
    struct anchors *a = &ui_of(t->doc)->bookmarks;
    if (!a->n) return;
    size_t ls = line_start(t->doc, line_of(t->doc, t->cursor)), i = anchors_find(a, ls);
    size_t to;
    if (dir > 0) {
        i += i < a->n && a->at[i] == ls;
        to = a->at[i < a->n ? i : 0];
    } else {
        to = a->at[i > 0 ? i - 1 : a->n - 1];
    }
    bookmark_go(s, t, line_of(t->doc, to));
}

/* The colour of line's modification marker in the border (KateIconBorder,
 * "modified line system"): ModifiedLines for a line changed since the file
 * was read, SavedLines for one changed and saved since; 0 for none, or for a
 * document without a file (Kate draws them only when it has a URL). */
static uint32_t line_marker(struct doc *d, size_t line)
{
    struct docui *u = ui_of(d);
    if (!LINE_MODIFICATION || !d->path[0] || !u->lines.n) return 0;
    lines_sync(d);
    size_t at = line_start(d, line), i = anchors_find(&u->lines, at);
    if (i >= u->lines.n || u->lines.at[i] != at) return 0;
    return u->lines.flag[i] & LINE_MODIFIED ? C_MODIFIED : u->lines.flag[i] & LINE_SAVED ? C_SAVED : 0;
}

/* Go to Previous and Next Modified Line (toPrevModifiedLine,
 * toNextModifiedLine with findTouchedLine): the nearest line above or below
 * the cursor's that is modified or saved, its start; the selection cleared;
 * none there, nothing moves (no cycling). */
static void modified_line_step(int s, struct tab *t, int dir)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    lines_sync(d);
    size_t cur = line_start(d, line_of(d, t->cursor)), i = anchors_find(&u->lines, cur);
    if (dir > 0) {
        i += i < u->lines.n && u->lines.at[i] == cur;
        while (i < u->lines.n && !(u->lines.flag[i] & (LINE_MODIFIED | LINE_SAVED))) i++;
        if (i >= u->lines.n) return;
    } else {
        do {
            if (i == 0) return;
            i--;
        } while (!(u->lines.flag[i] & (LINE_MODIFIED | LINE_SAVED)));
    }
    move_to(s, t, u->lines.at[i], 0, 0);
    show_cursor(s, t, 0);
}

/*
 * The scrollbar's geometry: QScrollBar's value, maximum and page step in
 * visible (unfolded) lines, as KateViewInternal sets them.
 */
struct scroll_geom {
    size_t total, page, max, value;
    int x, y, w, h;          /* its cells */
    int doc_rows;            /* the mini-map: the rows its lines take (m_mapGroveRect) */
    int slider_a, slider_b;  /* the rows of the slider, [a, b) */
};

static int minimap_cols(void)
{
    int cw, ch, top;
    screen_cells(&cw, &ch, &top);
    int n = cw > 0 ? (MINIMAP_PX + cw / 2) / cw : 6;
    return n < 3 ? 3 : n;
}

static int scroll_cols(int s, const struct tab *t)
{
    if (!t || !t->scrollbar) return 0;
    struct edit_rect r = space_rect(s);
    int n = t->minimap ? minimap_cols() : 1;
    return n > r.w / 4 ? r.w / 4 : n;
}

static void scroll_geometry(int s, struct tab *t, struct scroll_geom *g)
{
    struct edit_rect r = space_rect(s);
    int text_y, text_h;
    text_rows(s, &text_y, &text_h);
    g->w = scroll_cols(s, t);
    g->x = r.x + r.w - g->w;
    g->y = text_first_row(s);
    g->h = r.y + r.h - 1 - bar_rows(s) - g->y;
    if (g->h < 1) g->h = 1;
    g->total = fold_visible_index(t, t->doc->buf.lines - 1) + 1;
    g->page = (size_t)text_h;
    g->max = g->total > g->page ? g->total - g->page : 0;
    g->value = fold_visible_index(t, line_of(t->doc, t->top));
    if (g->value > g->max) g->max = g->value;
    size_t range = g->max + g->page;
    int rows = g->h;
    if (t->minimap) {
        /* At most two pixel rows a line: a short document takes the top. */
        int cw, ch, top;
        screen_cells(&cw, &ch, &top);
        long px = (long)g->total * 2, groove = (long)g->h * (ch > 0 ? ch : 1);
        rows = px >= groove ? g->h : (int)((px + (ch > 0 ? ch : 1) - 1) / (ch > 0 ? ch : 1));
        if (rows < 1) rows = 1;
    }
    g->doc_rows = rows;
    g->slider_a = (int)(g->value * (size_t)rows / range);
    g->slider_b = (int)((g->value + g->page) * (size_t)rows / range);
    if (g->slider_b > rows) g->slider_b = rows;
    if (g->slider_b <= g->slider_a) g->slider_b = g->slider_a + 1;
    if (g->slider_b > g->h) {
        g->slider_b = g->h;
        g->slider_a = g->slider_b - 1;
    }
}

/* The view scrolled to visible line value (the cursor stays). */
static void scroll_to(int s, struct tab *t, long value)
{
    struct scroll_geom g;
    scroll_geometry(s, t, &g);
    value = value < 0 ? 0 : (size_t)value > g.max ? (long)g.max : value;
    t->top = line_start(t->doc, fold_document_line(t, (size_t)value));
    t->sub = 0;
    clamp_top(s, t);
}

/* A cell row's first visible line and its count. */
static void minimap_lines(const struct scroll_geom *g, int row, size_t *first, size_t *count)
{
    size_t a = (size_t)row * g->total / (size_t)g->doc_rows, b = (size_t)(row + 1) * g->total / (size_t)g->doc_rows;
    *first = a;
    *count = b > a ? b - a : 1;
}

static void minimap_build(int s, struct tab *t, const struct scroll_geom *g, struct minimap_cache *c)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    int cols = g->w - 1, rows = g->doc_rows;
    size_t cells = (size_t)rows * (size_t)g->w;
    uint32_t *out = malloc(cells * sizeof *out);
    unsigned char *marks = calloc((size_t)rows, 1);
    if (!out || !marks || cols < 1) {
        free(out);
        free(marks);
        return;
    }
    int per_col = (MINIMAP_CHARS + cols - 1) / cols;
    enum { COLOURS = 8 };
    struct column {
        uint32_t colour[COLOURS];
        int count[COLOURS], ncolours, filled;
    } *column = calloc((size_t)cols, sizeof *column);
    if (!column) {
        free(out);
        free(marks);
        return;
    }
    for (int r = 0; r < rows; r++) {
        size_t first, count, slots = 0;
        minimap_lines(g, r, &first, &count);
        size_t step = count > MINIMAP_SAMPLE ? count / MINIMAP_SAMPLE : 1;
        memset(column, 0, (size_t)cols * sizeof *column);
        for (size_t i = 0; i < count; i += step) {
            size_t line = fold_document_line(t, first + i), ls = line_start(d, line), le = line_end(d, line);
            size_t end = le - ls > MINIMAP_CHARS * 4 ? ls + MINIMAP_CHARS * 4 : le;
            uint32_t *colour = d->syntax && end > ls ? colours_of(d, ls, end) : NULL;
            int x = 0;
            slots += (size_t)per_col;
            for (size_t p = ls; p < end && x < MINIMAP_CHARS;) {
                int w;
                uint32_t cp;
                size_t next = cluster(d, p, end, x, &w, &cp);
                if (cp != ' ' && cp != '\t') {
                    struct column *k = &column[x / per_col < cols ? x / per_col : cols - 1];
                    uint32_t fg = colour ? colour[p - ls] : C_TEXT;
                    int j = 0;
                    while (j < k->ncolours && k->colour[j] != fg) j++;
                    if (j == k->ncolours && j < COLOURS) {
                        k->colour[j] = fg;
                        k->ncolours++;
                    }
                    if (j < k->ncolours) k->count[j]++;
                    k->filled++;
                }
                x += w;
                p = next;
            }
            free(colour);
        }
        for (int c = 0; c < cols; c++) {
            const struct column *k = &column[c];
            uint32_t bg = C_BG;
            if (k->filled) {
                int best = 0;
                for (int j = 1; j < k->ncolours; j++) {
                    if (k->count[j] > k->count[best]) best = j;
                }
                /* As dense as its characters are, half of them full. */
                size_t amount = slots ? (size_t)k->filled * 2 * 255 / slots : 255;
                bg = colour_mix(C_BG, k->colour[best], amount > 255 ? 255 : amount < 96 ? 96 : (int)amount);
            }
            out[(size_t)r * (size_t)g->w + 1 + (size_t)c] = bg;
        }
        out[(size_t)r * (size_t)g->w] = C_BG;
    }
    free(column);
    /* The line modification markers, as the margin of the mini-map's pixmap. */
    if (d->buf.lines < 50000) {
        lines_sync(d);
        for (size_t i = 0; i < u->lines.n; i++) {
            size_t vis = fold_visible_index(t, line_of(d, u->lines.at[i]));
            int r = (int)(vis * (size_t)rows / (g->total ? g->total : 1));
            if (r < rows) marks[r] |= u->lines.flag[i];
        }
    }
    free(c->cells);
    free(c->marks);
    *c = (struct minimap_cache){ d, u->version, g->total, rows, g->w, g->h, 0, out, marks };
    (void)s;
}

/* The rows of the marks (bookmarks; Find All's and Replace All's matches as
 * SearchMatch marks, in the normal text colour) over rows cells. */
static void scroll_marks(struct tab *t, const struct scroll_geom *g, int rows, uint32_t *mark)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    size_t lines = g->total > 1 ? g->total - 1 : 1;
    for (int r = 0; r < rows; r++) mark[r] = 0;
    for (size_t i = 0; i < u->nhl; i++) {
        size_t vis = fold_visible_index(t, line_of(d, u->hl[2 * i]));
        int r = (int)((double)(rows - 1) * (double)vis / (double)lines + 0.5);
        if (r >= 0 && r < rows) mark[r] = C_TEXT;
    }
    for (size_t i = 0; i < u->bookmarks.n; i++) {
        size_t vis = fold_visible_index(t, line_of(d, u->bookmarks.at[i]));
        int r = (int)((double)(rows - 1) * (double)vis / (double)lines + 0.5);
        if (r >= 0 && r < rows) mark[r] = C_BOOKMARK;
    }
}

static void draw_scrollbar(int s, struct tab *t)
{
    if (!scroll_cols(s, t)) return;
    struct scroll_geom g;
    scroll_geometry(s, t, &g);
    uint32_t *mark = t->marks ? calloc((size_t)g.h, sizeof *mark) : NULL;
    if (!t->minimap) {
        if (mark) scroll_marks(t, &g, g.h, mark);
        for (int r = 0; r < g.h; r++) {
            int on = r >= g.slider_a && r < g.slider_b;
            uint32_t bg = on ? C_SLIDER : C_GROOVE;
            if (mark && mark[r] && !on) {
                screen_fill(g.x, g.y + r, 1, 1, mark[r]);
            } else if (mark && mark[r]) {
                screen_fill(g.x, g.y + r, 1, 1, bg);
                screen_text(g.x, g.y + r, "-", mark[r], bg);
            } else {
                screen_fill(g.x, g.y + r, 1, 1, bg);
            }
        }
        free(mark);
        ui_hit(g.x, g.y, g.w, g.h, HIT_SCROLL + s);
        return;
    }
    struct minimap_cache *c = &minimap_cache[s];
    if (c->doc != t->doc || c->version != ui_of(t->doc)->version || c->total != g.total || c->rows != g.doc_rows ||
        c->cols != g.w || c->h != g.h || !c->cells) {
        minimap_build(s, t, &g, c);
    }
    if (mark) scroll_marks(t, &g, g.doc_rows, mark);
    uint32_t slider = colour_mix(C_BG, C_FOCUS, 50); /* the slider: the highlight colour, alpha 50 */
    for (int r = 0; r < g.h; r++) {
        int on = r >= g.slider_a && r < g.slider_b, in_doc = r < g.doc_rows && c->cells;
        for (int k = 0; k < g.w; k++) {
            uint32_t bg = in_doc ? c->cells[(size_t)r * (size_t)g.w + (size_t)k] : C_BG;
            if (k == 0) {
                unsigned char f = in_doc ? c->marks[r] : 0;
                bg = f & LINE_MODIFIED ? C_MODIFIED : f & LINE_SAVED ? C_SAVED : C_BG;
            } else if (mark && in_doc && mark[r] && k < g.w - 1) {
                bg = mark[r];
            } else if (on) {
                bg = bg == C_BG ? slider : colour_mix(bg, C_FOCUS, 50);
            } else if (in_doc) {
                bg = colour_mix(bg, C_BG, 110); /* the invisible sections faded */
            }
            screen_fill(g.x + k, g.y + r, 1, 1, bg);
        }
    }
    free(mark);
    ui_hit(g.x, g.y, g.w, g.h, HIT_SCROLL + s);
}

/* The scrollbar's or the mini-map's drag: the slider follows the pointer. */
static struct {
    int on, space;
    int row;                 /* where it was pressed */
    long value;              /* the value then */
} scroll_drag;

static int scroll_pointer(int id, const struct pointer_event *e)
{
    if (id < HIT_SCROLL || id >= HIT_SCROLL + MAX_SPACES || (e->button != BTN_LEFT && e->button != BTN_MIDDLE)) {
        return 0;
    }
    int s = id - HIT_SCROLL;
    struct tab *t = tab_of(s);
    preview_hide(); /* a press takes the text preview away */
    if (!t) return 1;
    struct scroll_geom g;
    scroll_geometry(s, t, &g);
    int row = e->row - g.y, rows = t->minimap ? g.doc_rows : g.h;
    size_t range = g.max + g.page;
    if (e->button == BTN_LEFT && row >= g.slider_a && row < g.slider_b) {
        scroll_drag.value = (long)g.value; /* the slider: dragged */
    } else if (!t->minimap || row < rows) {
        /* The mini-map's left click, and the middle one, jump straight there,
         * the clicked line in the middle of the view; a left click in the
         * scrollbar's groove does as KDE's default (to the clicked location). */
        long value = (long)(((double)row + 0.5) / rows * (double)range) - (long)g.page / 2;
        scroll_to(s, t, value);
        scroll_geometry(s, t, &g);
        scroll_drag.value = (long)g.value;
    } else {
        return 1;
    }
    scroll_drag.on = 1;
    scroll_drag.space = s;
    scroll_drag.row = e->row;
    active = s;
    drag.mode = DRAG_NONE;
    return 1;
}

static int scroll_dragging(const struct pointer_event *e)
{
    if (!scroll_drag.on) return 0;
    struct tab *t = tab_of(scroll_drag.space);
    if (!t || ((e->button == BTN_LEFT || e->button == BTN_MIDDLE) && !e->pressed)) {
        scroll_drag.on = 0;
        return 1;
    }
    if (!e->button && !e->wheel) {
        struct scroll_geom g;
        scroll_geometry(scroll_drag.space, t, &g);
        int rows = t->minimap ? g.doc_rows : g.h;
        double per_row = (double)(g.max + g.page) / (rows > 0 ? rows : 1);
        scroll_to(scroll_drag.space, t, scroll_drag.value + (long)((e->row - scroll_drag.row) * per_row));
        return 1;
    }
    return e->button != 0;
}

/*
 * Scroll Bar Preview (on by default): the pointer over the scrollbar's or
 * the mini-map's groove shows, after 250 ms (m_delayTextPreviewTimer) and at
 * once while it is shown, the lines at that place (KateScrollBar::
 * showTextPreview, KateTextPreview): the line at the pointer's share of the
 * visible lines in the middle, the end of the document filling the last
 * rows; a box half the view wide and a fifth of its height high, at the left
 * of the scrollbar, at the pointer's height within it. Kate draws it at 0.75
 * of the font's size: the box has as many lines as Kate's shows, each at
 * the size of the text. A press, leaving the groove, or a document that fits
 * (no range to scroll) shows none.
 */
static struct {
    int space, row, y;       /* where the pointer is: its space's scrollbar, its row and pixel row */
    int shown;
    double due;              /* when it shows; 0: not pending */
} preview = { .space = -1 };

static void preview_hide(void)
{
    preview.space = -1;
    preview.shown = 0;
    preview.due = 0;
}

static void scroll_hover(int id, const struct pointer_event *e)
{
    if (!SCROLL_PREVIEW || id < HIT_SCROLL || id >= HIT_SCROLL + MAX_SPACES || scroll_drag.on || (e->buttons & 1)) {
        preview_hide();
        return;
    }
    preview.space = id - HIT_SCROLL;
    preview.row = e->row;
    preview.y = e->y;
    if (!preview.shown && !preview.due) preview.due = now() + 0.25; /* showTextPreviewDelayed */
}

static int preview_timeout(void)
{
    if (!preview.due) return -1;
    double left = preview.due - now();
    return left <= 0 ? 0 : (int)(left * 1000) + 1;
}

/* A line of the preview: from its start, not wrapped, in its colours. */
static void preview_line(struct doc *d, size_t line, int x, int y, int w)
{
    size_t ls = line_start(d, line), le = line_end(d, line);
    size_t end = le - ls > (size_t)w * 4 + 64 ? ls + (size_t)w * 4 + 64 : le;
    uint32_t *colour = end > ls ? colours_of(d, ls, end) : NULL;
    int cx = 0;
    for (size_t p = ls; p < end && cx < w;) {
        int cells;
        uint32_t cp;
        size_t next = cluster(d, p, end, cx, &cells, &cp);
        if (cx + cells > w) break;
        char glyph[64];
        size_t o = 0;
        for (size_t q = p; q < next && o + 8 < sizeof glyph;) {
            size_t k;
            uint32_t c = buffer_char(&d->buf, q, &k);
            if (c == '\t') {
                /* blank, as the background */
            } else if (c < 0x20 || c == 0x7F || (k == 1 && byte_at(d, q) >= 0x80)) {
                glyph[o++] = '?';
            } else {
                o += (size_t)text_put(glyph + o, c);
            }
            q += k ? k : 1;
        }
        glyph[o] = '\0';
        if (o) screen_text(x + cx, y, glyph, colour ? colour[p - ls] : C_TEXT, C_BG);
        cx += cells;
        p = next;
    }
    free(colour);
}

static void preview_draw(void)
{
    if (preview.space < 0) return;
    if (!preview.shown) {
        if (!preview.due || now() < preview.due) return;
        preview.shown = 1;
        preview.due = 0;
    }
    int s = preview.space;
    struct tab *t = s < nspaces ? tab_of(s) : NULL;
    if (!t || !scroll_cols(s, t)) {
        preview_hide();
        return;
    }
    struct scroll_geom g;
    scroll_geometry(s, t, &g);
    if (g.max == 0) return; /* minimum() == maximum() */
    int rows = t->minimap ? g.doc_rows : g.h, cw, ch, top;
    screen_cells(&cw, &ch, &top);
    double pos = cw > 0 && ch > 0 ? (double)(preview.y - top - g.y * ch) / ((double)rows * ch) :
                                    ((double)(preview.row - g.y) + 0.5) / rows;
    if (pos < 0 || pos >= 1) {
        preview_hide(); /* outside the groove */
        return;
    }
    double line = pos * (double)g.total; /* startLine: posInPercent * visibleLines */
    struct edit_rect r = space_rect(s);
    int n = (r.h * 4 + 14) / 15, w = r.w / 2; /* ceil(height / 5 / 0.75) lines */
    n = n > r.h - 2 ? r.h - 2 : n;
    if (n < 1 || w < 8) return;
    int bh = n + 2, x = g.x - w, y = preview.row - bh / 2;
    y = y < g.y ? g.y : y;
    y = y > g.y + g.h - bh ? g.y + g.h - bh : y;
    y = y < 0 ? 0 : y;
    x = x < 0 ? 0 : x;
    long first = (long)line - (n + 1) / 2; /* centered: m_line - ceil(lineCount / 2) */
    if (first + n > (long)g.total) first = (long)g.total - n; /* the end of the document fills it */
    first = first < 0 ? 0 : first;
    screen_fill(x, y, w, bh, C_SEPARATOR); /* QFrame::StyledPanel */
    screen_fill(x + 1, y + 1, w - 2, n, C_BG);
    for (int i = 0; i < n && (size_t)(first + i) < g.total; i++) {
        preview_line(t->doc, fold_document_line(t, (size_t)(first + i)), x + 1, y + 1 + i, w - 2);
    }
}

/*
 * Auto Brackets (on by default) and Chars To Enclose Selection (<>(){}[]'"),
 * as typeChars, skipAutoBrace and backspace do them.
 */
static int ab_closing(int c)
{
    return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == '\'' || c == '"' ? c : 0;
}

static int ab_mirror(int c)
{
    const char *pairs = "()[]{}<>";
    const char *p = strchr(pairs, c);
    return p && c ? pairs[(p - pairs) ^ 1] : c;
}

/* skipAutoBrace: a ' in text that is spell checked (comments, strings, a
 * document without a language) is an apostrophe; a quote that balances the
 * ones (not escaped) before it on the line closes them. */
static int ab_skip(struct doc *d, int closing, size_t pos)
{
    if (closing != '\'' && closing != '"') return 0;
    if (closing == '\'') {
        if (!d->syntax || d->lang == syntax_by_name("Markdown")) return 1;
        if (pos >= 2) {
            const char *capture = syntax_capture_at(d->syntax, &d->buf, pos - 2);
            if (strncmp(capture, "comment", 7) == 0 || strncmp(capture, "string", 6) == 0 ||
                strncmp(capture, "text", 4) == 0 || strncmp(capture, "markup", 6) == 0) {
                return 1;
            }
        }
    }
    size_t ls = line_start(d, line_of(d, pos)), count = 0;
    for (size_t p = ls; p < pos; p++) {
        if (byte_at(d, p) != closing) continue;
        size_t slashes = 0;
        while (p - slashes > ls && byte_at(d, p - slashes - 1) == '\\') slashes++;
        count += slashes % 2 == 0;
    }
    return count % 2 == 0;
}

static int auto_brackets(int s, struct tab *t, const char *text, size_t len, int *closing)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    int typed = (unsigned char)text[0];
    *closing = 0;
    if (typed == ')' || typed == ']' || typed == '}') {
        size_t other;
        if (t->cursor < line_end(d, line_of(d, t->cursor)) && byte_at(d, t->cursor) == typed &&
            match_bracket(d, t->cursor, &other)) {
            move_to(s, t, t->cursor + 1, 0, 0); /* a balanced closing bracket is typed over */
            return 1;
        }
    }
    if (len == 1) {
        *closing = ab_closing(typed);
        if (u->autobrace && u->ab_char == typed && t->cursor >= u->ab_start && t->cursor < u->ab_end &&
            byte_at(d, t->cursor) == typed) {
            u->autobrace = 0; /* the closing one of the pair just made */
            move_to(s, t, t->cursor + 1, 0, 0);
            return 1;
        }
    }
    if (has_sel(t) && !*closing && typed && strchr("<>(){}[]'\"", typed)) {
        *closing = ab_mirror(typed);
    }
    if (has_sel(t) && *closing) {
        /* The selection enclosed, and still selected. */
        size_t a, b;
        sel_range(t, &a, &b);
        char c = (char)*closing;
        doc_begin(d, t->cursor, t->anchor);
        doc_insert(d, b, &c, 1);
        doc_insert(d, a, text, len);
        t->anchor = a + len;
        t->cursor = b + len;
        doc_end(d, t->cursor, t->anchor);
        edited(t);
        show_cursor(s, t, 0);
        *closing = 0;
        return 1;
    }
    return 0;
}

/* The closing bracket after what was typed, unless a letter or a digit
 * follows; the cursor stays before it. */
static void auto_bracket_close(struct tab *t, int closing)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    size_t pos = t->cursor;
    if (ab_skip(d, closing, pos)) return;
    u->ab_char = closing;
    size_t le = line_end(d, line_of(d, pos)), n;
    uint32_t next = pos < le ? buffer_char(&d->buf, pos, &n) : ' ';
    if (pos < le && !iswspace((wint_t)next) && iswalnum((wint_t)next)) return;
    char c = (char)closing;
    doc_insert(d, pos, &c, 1);
    t->cursor = pos;
    u->autobrace = 1;
    u->ab_start = pos - 1;
    u->ab_end = pos + 1;
}

/* Backspace over the opening bracket of the pair just made takes both. */
static void auto_bracket_backspace(struct tab *t)
{
    struct doc *d = t->doc;
    struct docui *u = ui_of(d);
    if (u->autobrace && u->ab_end == u->ab_start + 1 && t->cursor == u->ab_start && t->cursor < doc_len(d)) {
        u->autobrace = 0;
        doc_remove(d, t->cursor, 1);
    }
}

/* checkCursorForAutobrace: the pair is forgotten when the cursor leaves it. */
static void auto_bracket_check(void)
{
    struct tab *t = cur_tab();
    if (!t) return;
    struct docui *u = ui_of(t->doc);
    if (u->autobrace && (t->cursor < u->ab_start || t->cursor >= u->ab_end)) u->autobrace = 0;
}
