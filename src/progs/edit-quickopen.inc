/*
 * Quick Open (the handbook's "Quick Open" and View > Quick Open, Ctrl+Alt+O;
 * kate's apps/lib/quickopen and kfts_fuzzy_match.h). Its list: the open
 * documents, those of the views the last used first, then the files of the
 * current project as the Projects tree lists them (for a file outside version
 * control, its folder's), each once; Current Project and Fuzzy Filtering are
 * its defaults. What is typed filters it: only the file name is matched,
 * unless the text has a "/", when its last part is matched with the name and
 * all of it with the path in the project; the best matches first, open
 * documents and files at the project's top ahead. ":LINE:COLUMN" after the
 * text goes there. Up, Down, Page Up and Page Down move in the list; the
 * other keys, Home and End with them, are the text field's
 * (KateQuickOpen::eventFilter's forward2list); Enter or a click opens the
 * file in the active view, Esc closes.
 * Included in edit.c.
 */
#define QUICK_OPEN_FILES 50000 /* files listed of one project, at most */
#define FUZZY_MAX        256   /* kfts: maxMatches */

/* kfts::fuzzy_match, over code points; positions of the matches in m. */
static int fuzzy_recursive(const uint32_t *pat, size_t np, size_t pi, const uint32_t *str, size_t ns, size_t si,
                           int *score, const int *src, int *m, int next, int *total, int *count)
{
    if (++*count >= 10 || pi == np || si == ns) return 0; /* recursionLimit */
    int recursive = 0, best[FUZZY_MAX], best_score = 0, first = 1;
    uint32_t want = towlower((wint_t)pat[pi]);
    while (pi < np && si < ns) {
        if (want == (uint32_t)towlower((wint_t)str[si])) {
            if (next >= FUZZY_MAX) return 0;
            if (first && src) {
                memcpy(m, src, (size_t)next * sizeof *m);
                first = 0;
            }
            int sub[FUZZY_MAX], sub_score = 0;
            if (fuzzy_recursive(pat, np, pi, str, ns, si + 1, &sub_score, m, sub, next, total, count)) {
                if (!recursive || sub_score > best_score) {
                    memcpy(best, sub, sizeof best);
                    best_score = sub_score;
                }
                recursive = 1;
            }
            m[next++] = (int)si;
            pi++;
            want = pi < np ? towlower((wint_t)pat[pi]) : 0;
        }
        si++;
    }
    int matched = pi == np;
    if (matched) {
        int non_begin = 10, in_separator = 0, all_consecutive = 1;
        *score = 100 - ((int)ns - next); /* unmatchedLetterPenalty */
        if (m[0] == 0) {
            *score += 15 + 25; /* firstLetterBonus + separatorBonus */
            in_separator = 1;
        } else {
            uint32_t before = str[m[0] - 1], cur = str[m[0]];
            if (before == '_' || before == ' ' || (iswlower((wint_t)before) && iswupper((wint_t)cur))) {
                *score += 15 - 3 + 25;
                in_separator = 1;
            } else {
                non_begin += 4;
            }
            *score -= m[0];
        }
        for (int i = 1; i < next; i++) {
            int at = m[i], prev = m[i - 1];
            if (at == prev + 1) {
                if (i == at) {
                    *score += 20; /* sequentialBonus */
                } else if (in_separator) {
                    *score += 20 - 3;
                } else {
                    *score += non_begin;
                    non_begin += 4;
                }
            } else {
                all_consecutive = 0;
                *score += -(at - prev) - 2;
                in_separator = 0;
                non_begin = 10;
            }
            uint32_t before = str[at - 1], cur = str[at];
            if (before == '_' || before == ' ' || (iswlower((wint_t)before) && iswupper((wint_t)cur))) {
                *score += 25;
                in_separator = 1;
            }
        }
        if (all_consecutive && next >= 4) *score *= 2;
    }
    *total = next;
    if (recursive && (!matched || best_score > *score)) {
        memcpy(m, best, sizeof best);
        *score = best_score;
        return 1;
    }
    return matched;
}

static size_t utf8_points(const char *s, uint32_t *out, size_t max)
{
    size_t n = 0;
    for (const char *p = s; *p && n < max;) {
        size_t k;
        out[n++] = text_decode(p, strlen(p), &k);
        p += k ? k : 1;
    }
    return n;
}

/* fuzzy_match(pattern, str, score): 1 and the score, and the positions (in
 * code points) of the matched characters in m when m is given. */
static int fuzzy_match(const char *pattern, const char *text, int *score, int *m, int *nm)
{
    uint32_t pat[FUZZY_MAX], str[1024];
    size_t np = utf8_points(pattern, pat, FUZZY_MAX), ns = utf8_points(text, str, 1024);
    int scratch[FUZZY_MAX], dummy;
    if (!m) m = scratch;
    if (!nm) nm = &dummy;
    *nm = 0;
    *score = 0;
    if (np == 0) return 1;
    if (np == 1) {
        for (size_t i = 0; i < ns; i++) {
            if (towlower((wint_t)str[i]) == towlower((wint_t)pat[0])) {
                *score = 250 - (int)i + (pat[0] == str[i]);
                m[0] = (int)i;
                *nm = 1;
                return 1;
            }
        }
        return 0;
    }
    size_t pi = 0;
    for (size_t si = 0; si < ns && pi < np; si++) {
        pi += towlower((wint_t)str[si]) == towlower((wint_t)pat[pi]); /* the quick test first */
    }
    if (pi < np) return 0;
    int count = 0;
    return fuzzy_recursive(pat, np, 0, str, ns, 0, score, NULL, m, 0, nm, &count);
}

struct qo_entry {
    char name[256];          /* the file's name, or the document's (Untitled) */
    char *path;              /* the full path, "" for a document without a file */
    const char *rel;         /* the path shown and matched: without the project's folder */
    struct doc *doc;         /* an open document's */
    int score, index;
};

static struct {
    struct qo_entry *e;
    int n, size;
    int *shown, nshown;      /* the entries filtered, in order */
    int sel, top;
    char base[PATH_MAX + 1]; /* the project's folder and its "/" */
} qo;

static void qo_add(struct doc *d, const char *path)
{
    if (qo.n == qo.size) {
        int size = qo.size ? qo.size * 2 : 256;
        struct qo_entry *more = realloc(qo.e, (size_t)size * sizeof *more);
        if (!more) return;
        qo.e = more;
        qo.size = size;
    }
    struct qo_entry *e = &qo.e[qo.n];
    memset(e, 0, sizeof *e);
    if (!(e->path = strdup(path))) return;
    e->doc = d;
    e->index = qo.n;
    if (d && !d->path[0]) {
        doc_name(d, e->name, sizeof e->name);
    } else {
        snprintf(e->name, sizeof e->name, "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    }
    size_t bl = strlen(qo.base);
    e->rel = bl && strncmp(e->path, qo.base, bl) == 0 ? e->path + bl : e->path;
    qo.n++;
}

static int qo_seen(const struct doc *d, const char *path)
{
    for (int i = 0; i < qo.n; i++) {
        if ((d && qo.e[i].doc == d) || (!d && path[0] && strcmp(qo.e[i].path, path) == 0)) return 1;
    }
    return 0;
}

static int qo_tab_by_use(const void *a, const void *b)
{
    long x = (*(const struct tab *const *)a)->used, y = (*(const struct tab *const *)b)->used;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* KateQuickOpenModel::refresh. */
static void qo_build(void)
{
    struct tab *t = cur_tab();
    struct project *p = project; /* the current project: the active document's, else the tree's */
    if (t && t->doc->path[0]) {
        p = ui_of(t->doc)->project ? ui_of(t->doc)->project : project_for(t->doc->path);
    }
    qo.base[0] = '\0';
    if (p && p->root[0] && strcmp(p->root, "/") != 0) snprintf(qo.base, sizeof qo.base, "%s/", p->root);
    int ntabs = 0;
    for (int s = 0; s < nspaces; s++) ntabs += spaces[s].ntabs;
    struct tab **views = malloc((size_t)(ntabs ? ntabs : 1) * sizeof *views);
    int nv = 0;
    for (int s = 0; views && s < nspaces; s++) {
        for (int i = 0; i < spaces[s].ntabs; i++) views[nv++] = &spaces[s].tabs[i];
    }
    if (nv > 1) qsort(views, (size_t)nv, sizeof *views, qo_tab_by_use);
    for (int i = 0; i < nv; i++) {
        if (!qo_seen(views[i]->doc, "")) qo_add(views[i]->doc, views[i]->doc->path);
    }
    free(views);
    for (int i = 0; i < ndocs; i++) {
        if (!qo_seen(docs[i], "")) qo_add(docs[i], docs[i]->path);
    }
    int opened = qo.n;
    char **files = NULL;
    int truncated, nf = p ? project_files(p, &files, QUICK_OPEN_FILES, &truncated) : 0;
    for (int i = 0; i < nf; i++) {
        int open = 0;
        for (int k = 0; k < opened && !open; k++) open = strcmp(qo.e[k].path, files[i]) == 0;
        if (!open) qo_add(NULL, files[i]);
    }
    if (nf > 0) project_files_free(files, nf);
}

static void qo_free(void)
{
    for (int i = 0; i < qo.n; i++) free(qo.e[i].path);
    free(qo.e);
    free(qo.shown);
    memset(&qo, 0, sizeof qo);
}

static int qo_by_score(const void *pa, const void *pb)
{
    const struct qo_entry *a = &qo.e[*(const int *)pa], *b = &qo.e[*(const int *)pb];
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    return a->index - b->index; /* stable */
}

static int qo_by_opened(const void *pa, const void *pb)
{
    const struct qo_entry *a = &qo.e[*(const int *)pa], *b = &qo.e[*(const int *)pb];
    if (!a->doc != !b->doc) return a->doc ? -1 : 1;
    return a->index - b->index;
}

/* The pattern: the text before its first ":". */
static void qo_pattern(const char *text, char *out, size_t size)
{
    snprintf(out, size, "%.*s", (int)strcspn(text, ":"), text);
}

/* QuickOpenFilterProxyModel::filterAcceptsRow, then the sort by score. */
static void qo_filter(const char *text)
{
    char pattern[1024];
    qo_pattern(text, pattern, sizeof pattern);
    free(qo.shown);
    qo.shown = malloc((size_t)(qo.n ? qo.n : 1) * sizeof *qo.shown);
    qo.nshown = 0;
    if (!qo.shown) return;
    int path = strchr(pattern, '/') != NULL;
    const char *name_pattern = path ? strrchr(pattern, '/') + 1 : pattern;
    for (int i = 0; i < qo.n; i++) {
        struct qo_entry *e = &qo.e[i];
        int score = 0, ok = 1;
        if (pattern[0]) {
            ok = !name_pattern[0] || fuzzy_match(name_pattern, e->name, &score, NULL, NULL);
            if (path && ok) {
                int path_score = 0;
                ok = fuzzy_match(pattern, e->rel, &path_score, NULL, NULL);
                score = ok ? score + path_score : 0;
            }
            if (ok) {
                score += e->doc ? score / 6 : 0;                             /* open documents */
                if (!path && strcmp(e->rel, e->name) == 0) score += (int)strlen(e->name); /* at the project's top */
            }
        }
        e->score = score;
        if (ok) qo.shown[qo.nshown++] = i;
    }
    qsort(qo.shown, (size_t)qo.nshown, sizeof *qo.shown, pattern[0] ? qo_by_score : qo_by_opened);
}

/* reselectFirst: the previous document when nothing is typed and there are
 * views of more than one, else the first. */
static void qo_reselect(const char *text)
{
    int views = 0;
    for (int s = 0; s < nspaces; s++) views += spaces[s].ntabs;
    qo.sel = views > 1 && qo.nshown > 1 && !text[0] ? 1 : 0;
    qo.top = 0;
}

struct qo_box {
    int x, y, w, h, list;    /* list: the rows of the list */
};

static void qo_geometry(struct qo_box *b)
{
    int cols = screen_cols(), rows = screen_rows() - 2, cw, ch, top;
    screen_cells(&cw, &ch, &top);
    cw = cw > 0 ? cw : 10;
    ch = ch > 0 ? ch : 20;
    int min_w = 500 / cw, min_h = 250 / ch, w = (int)(cols / 2.4), h = rows / 2;
    w = w < min_w ? min_w : w;
    h = h < min_h ? min_h : h;
    b->w = w > cols ? cols : w;
    b->h = h > rows ? rows : h;
    b->x = (cols - b->w) / 2;
    b->y = 2;
    b->list = b->h - 1;
}

static void qo_draw(const struct field *f)
{
    struct qo_box b;
    draw();
    ui_hits_clear();
    qo_geometry(&b);
    screen_fill(b.x, b.y, b.w, b.h, C_WINDOW);
    draw_field(b.x + 1, b.y, b.w - 2, f, 1, C_FIELD);
    if (qo.sel < qo.top) qo.top = qo.sel;
    if (qo.sel >= qo.top + b.list) qo.top = qo.sel - b.list + 1;
    char pattern[1024];
    qo_pattern(f->s, pattern, sizeof pattern);
    const char *name_pattern = strrchr(pattern, '/') ? strrchr(pattern, '/') + 1 : pattern;
    for (int r = 0; r < b.list && qo.top + r < qo.nshown; r++) {
        int i = qo.top + r, row = b.y + 1 + r, on = i == qo.sel;
        struct qo_entry *e = &qo.e[qo.shown[i]];
        uint32_t bg = on ? C_FOCUS : C_WINDOW;
        screen_fill(b.x, row, b.w, 1, bg);
        /* The name (an open document's as Kate's bold: the window text, a
         * project file's dimmer), the matched characters in the link colour,
         * then the folder in grey. */
        int m[FUZZY_MAX], nm = 0, score, x = b.x + 1, k = 0, c = 0;
        if (name_pattern[0]) fuzzy_match(name_pattern, e->name, &score, m, &nm);
        for (const char *p = e->name; *p && x < b.x + b.w - 1; c++) {
            size_t n;
            uint32_t cp = text_decode(p, strlen(p), &n);
            char one[8];
            one[text_put(one, cp)] = '\0';
            while (k < nm && m[k] < c) k++;
            uint32_t fg = k < nm && m[k] == c ? (on ? 0xFFFFFF : C_FOCUS) : on ? C_WINDOW : e->doc ? C_WTEXT : 0xBDC3C7;
            put_text(x, row, b.x + b.w - 1, one, fg, bg);
            x += screen_char_cells(cp);
            p += n ? n : 1;
        }
        char folder[PATH_MAX];
        snprintf(folder, sizeof folder, "%s", e->rel);
        size_t fl = strlen(folder), nl = strlen(e->name);
        if (fl > nl && folder[fl - nl - 1] == '/' && strcmp(folder + fl - nl, e->name) == 0) folder[fl - nl - 1] = '\0';
        put_text(x + 1, row, b.x + b.w - 1, folder, on ? C_WINDOW : C_DIM, bg);
        ui_hit(b.x, row, b.w, 1, HIT_QUICKOPEN + i);
    }
    screen_flush();
}

/* slotReturnPressed: the document shown, or the file opened, in the active
 * view; then the line and column after the text's ":". */
static void qo_open(int i, const char *text)
{
    if (i < 0 || i >= qo.nshown) return;
    struct qo_entry *e = &qo.e[qo.shown[i]];
    struct doc *d = e->doc;
    int alive = 0;
    for (int k = 0; d && k < ndocs; k++) alive |= docs[k] == d;
    if (alive) {
        show_doc(active, d);
        follow_project();
    } else if (e->path[0]) {
        d = open_file(e->path);
    }
    struct tab *t = cur_tab();
    const char *colon = strchr(text, ':');
    if (d && t && t->doc == d && colon) {
        char *end;
        long line = strtol(colon + 1, &end, 10), col = 1;
        if (end != colon + 1) {
            if (*end == ':') col = strtol(end + 1, NULL, 10);
            line = line < 1 ? 1 : (size_t)line > d->buf.lines ? (long)d->buf.lines : line;
            size_t ls = line_start(d, (size_t)line - 1);
            size_t at = ls + min_z(col > 1 ? (size_t)col - 1 : 0, buffer_line_length(&d->buf, (size_t)line - 1));
            t->cursor = t->anchor = at;
            show_cursor(active, t, 1);
        }
    }
    bar.focus = FOCUS_VIEW;
    tree_focus = 0;
}

static void quick_open(void)
{
    struct field f = { 0 };
    qo_build();
    qo_filter("");
    qo_reselect("");
    for (;;) {
        qo_draw(&f);
        struct qo_box b;
        qo_geometry(&b);
        int key = key_poll_timeout(-1, -1), mods = keys_modifiers();
        if (key == K_SHOWN || key == 0) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if ((e->button == BTN_LEFT || e->button == BTN_RIGHT) && e->pressed) {
                int id = ui_hit_at(e->col, e->row);
                if (e->button == BTN_LEFT && id >= HIT_QUICKOPEN && id < HIT_QUICKOPEN + qo.nshown) {
                    qo.sel = id - HIT_QUICKOPEN; /* a click opens (Kate's clicked) */
                    qo_open(qo.sel, f.s);
                    break;
                }
                if (e->col < b.x || e->col >= b.x + b.w || e->row < b.y || e->row >= b.y + b.h) break; /* focus out */
            } else if (e->wheel) {
                qo.sel -= e->wheel * WHEEL_LINES;
                qo.sel = qo.sel < 0 ? 0 : qo.sel >= qo.nshown ? qo.nshown - 1 : qo.sel;
            }
            continue;
        }
        if (key == K_CTRL_SPACE) {
            ime_cycle_mode();
            say_toast(ime_mode_desc());
            continue;
        }
        ime_set_focus(1);
        int taken = ime_process_key(key);
        const char *commit = ime_get_commit();
        if (commit && commit[0]) {
            field_insert(&f, commit, strlen(commit));
            qo_filter(f.s);
            qo_reselect(f.s);
        }
        if (taken) continue;
        if (key == 27) break;
        if (key == '\n') {
            qo_open(qo.sel, f.s);
            break;
        }
        key = real_key(key, mods);
        if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN) {
            int page = b.list > 1 ? b.list - 1 : 1;
            qo.sel = key == K_UP ? qo.sel - 1 : key == K_DOWN ? qo.sel + 1 : key == K_PGUP ? qo.sel - page :
                     qo.sel + page;
            qo.sel = qo.sel >= qo.nshown ? qo.nshown - 1 : qo.sel;
            qo.sel = qo.sel < 0 ? 0 : qo.sel;
            continue;
        }
        int changed;
        char before[sizeof f.s];
        snprintf(before, sizeof before, "%s", f.s);
        field_key(&f, key, mods, &changed);
        if (changed) {
            char a[1024], b2[1024];
            qo_pattern(before, a, sizeof a);
            qo_pattern(f.s, b2, sizeof b2);
            if (strcmp(a, b2) != 0) qo_filter(f.s); /* line:column typed: the list stays */
            qo_reselect(f.s);
        }
    }
    qo_free();
}
