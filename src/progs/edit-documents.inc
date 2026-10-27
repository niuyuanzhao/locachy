/*
 * The Documents tool view and Ctrl+Tab's list of documents, two of Kate's
 * default plugins (apps/lib/katepluginmanager.cpp: katefiletreeplugin,
 * tabswitcherplugin). Included in edit.c.
 *
 * Documents (addons/filetree, the handbook's "The Documents List"): in the
 * left sidebar, its toolbar shown; Tree Mode, the folders around the open
 * documents (the deepest folder holding a group of them a root, roots of the
 * same name told apart by their parents, as KateFileTreeModel builds them),
 * folders first, sorted by document name; the active document selected and
 * its folders opened; a modified one with its mark; the recently viewed and
 * edited ones shaded. A click shows a document; the context menu closes
 * documents, reloads them, copies their location, and sets the view mode
 * and the sorting. Alt+Up and Alt+Down are Previous and Next Document.
 *
 * Document Switcher (addons/tabswitcher): Ctrl+Tab and Ctrl+Shift+Tab open
 * the list of documents, the last used first, at the next or the previous
 * one; with Ctrl held, Tab and Shift+Tab, Up, Down, Home, End and a letter
 * move in it, Ctrl+W closes the selected document, Esc cancels; Ctrl's
 * release, or a click, switches to the selected one.
 */
enum { DOCS_SORT_NAME, DOCS_SORT_PATH, DOCS_SORT_OPENING };
static int docs_list_mode;   /* listMode: off, Tree Mode */
static int docs_sort;        /* sortRole: Qt::DisplayRole, the document's name */
static int docs_shading = 1; /* shadingEnabled */
static int docs_toolbar = 1; /* showToolbar */
static int docs_top;
static char **docs_open_dirs;   /* the folders expanded */
static int ndocs_open_dirs;
static struct doc *docs_shown_active; /* the active document the last time the list was made */

#define DOCS_MAX_ROWS 2048
#define DOCS_EMPTY    DOCS_MAX_ROWS /* the list's empty part */
struct docrow {
    struct doc *doc;         /* NULL: a folder */
    char *path;              /* the folder's */
    char name[256];
    int depth, order;
};
static struct docrow docs_rows[DOCS_MAX_ROWS], docs_scratch[DOCS_MAX_ROWS];
static int ndocs_rows;

static int natural_cmp(const char *a, const char *b)
{
    /* QCollator with case insensitivity and numeric mode. */
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            size_t na = 0, nb = 0;
            while (isdigit((unsigned char)a[na])) na++;
            while (isdigit((unsigned char)b[nb])) nb++;
            if (na != nb) return na < nb ? -1 : 1;
            int c = strncmp(a, b, na);
            if (c) return c;
            a += na;
            b += nb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void doc_dir(const struct doc *d, char *out, size_t size)
{
    snprintf(out, size, "%s", d->path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        out[0] = '\0';
    } else {
        slash[slash == out] = '\0';
    }
}

/* path is dir or in it. */
static int path_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (strcmp(dir, "/") == 0) return path[0] == '/';
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static const char *last_part(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static int docs_expanded(const char *path)
{
    for (int i = 0; i < ndocs_open_dirs; i++) {
        if (strcmp(docs_open_dirs[i], path) == 0) return 1;
    }
    return 0;
}

static void docs_set_expanded(const char *path, int on)
{
    for (int i = 0; i < ndocs_open_dirs; i++) {
        if (strcmp(docs_open_dirs[i], path) == 0) {
            if (!on) {
                free(docs_open_dirs[i]);
                docs_open_dirs[i] = docs_open_dirs[--ndocs_open_dirs];
            }
            return;
        }
    }
    char **more = on ? realloc(docs_open_dirs, (size_t)(ndocs_open_dirs + 1) * sizeof *more) : NULL;
    if (more) {
        docs_open_dirs = more;
        if ((docs_open_dirs[ndocs_open_dirs] = strdup(path))) ndocs_open_dirs++;
    }
}

/* The roots of Tree Mode: the folders of documents no other document's
 * folder holds; two of the same name replaced by their parents. */
static int docs_roots(char ***out)
{
    char **roots = calloc((size_t)(ndocs ? ndocs : 1), sizeof *roots);
    int n = 0;
    for (int i = 0; roots && i < ndocs; i++) {
        if (!docs[i]->path[0]) continue;
        char dir[PATH_MAX];
        doc_dir(docs[i], dir, sizeof dir);
        int held = 0;
        for (int k = 0; k < ndocs && !held; k++) {
            char other[PATH_MAX];
            if (k == i || !docs[k]->path[0]) continue;
            doc_dir(docs[k], other, sizeof other);
            held = strcmp(other, dir) != 0 && path_under(dir, other);
        }
        for (int k = 0; k < n && !held; k++) held = strcmp(roots[k], dir) == 0;
        if (!held && (roots[n] = strdup(dir))) n++;
    }
    for (int round = 0; roots && round < 64; round++) {
        int changed = 0;
        for (int i = 0; i < n && !changed; i++) {
            for (int k = i + 1; k < n && !changed; k++) {
                if (strcmp(last_part(roots[i]), last_part(roots[k])) != 0) continue;
                for (int j = 0; j < 2; j++) {
                    char *r = roots[j ? k : i], *slash = strrchr(r, '/');
                    if (slash && slash != r) {
                        *slash = '\0';
                        changed = 1;
                    }
                }
            }
        }
        if (!changed) break;
        /* Roots now in another root are its folders. */
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < n; k++) {
                if (k != i && roots[i] && roots[k] && path_under(roots[k], roots[i])) {
                    free(roots[k]);
                    roots[k] = NULL;
                }
            }
        }
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (roots[i]) roots[w++] = roots[i];
        }
        n = w;
    }
    *out = roots;
    return roots ? n : 0;
}

struct docchild {
    struct doc *doc;
    char *path;
    const char *name;
    int order;
};

static int docchild_cmp(const void *pa, const void *pb)
{
    const struct docchild *a = pa, *b = pb;
    if (!a->doc != !b->doc) return a->doc ? 1 : -1; /* folders first */
    if (docs_sort == DOCS_SORT_OPENING) return a->order - b->order;
    if (docs_sort == DOCS_SORT_PATH) {
        int c = strcmp(a->doc ? a->doc->path : a->path, b->doc ? b->doc->path : b->path);
        if (c) return c;
    }
    int c = natural_cmp(a->name, b->name);
    return c ? c : a->order - b->order;
}

static void docs_row(struct docrow *rows, int *n, int max, struct doc *d, const char *path, const char *name, int depth,
                     int order)
{
    if (*n >= max) return;
    struct docrow *r = &rows[(*n)++];
    r->doc = d;
    r->path = path ? strdup(path) : NULL;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->depth = depth;
    r->order = order;
}

/* The contents of folder dir in Tree Mode: its documents, and its folders
 * that hold documents. */
static void docs_folder(struct docrow *rows, int *n, int max, const char *dir, int depth, int all, struct doc *reveal)
{
    struct docchild *kids = calloc((size_t)ndocs + 1, sizeof *kids);
    char **names = calloc((size_t)ndocs + 1, sizeof *names);
    int nk = 0;
    size_t dl = strcmp(dir, "/") == 0 ? 0 : strlen(dir);
    for (int i = 0; kids && names && i < ndocs; i++) {
        struct doc *d = docs[i];
        if (!d->path[0] || !path_under(d->path, dir)) continue;
        const char *rest = d->path + dl + 1, *slash = strchr(rest, '/');
        if (!slash) {
            kids[nk++] = (struct docchild){ d, NULL, last_part(d->path), i };
            continue;
        }
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%.*s", (int)(slash - d->path), d->path);
        int k = 0;
        while (k < nk && !(kids[k].path && strcmp(kids[k].path, sub) == 0)) k++;
        if (k < nk) {
            if (i < kids[k].order) kids[k].order = i;
            continue;
        }
        if (!(names[nk] = strdup(sub))) continue;
        kids[nk] = (struct docchild){ NULL, names[nk], last_part(names[nk]), i };
        nk++;
    }
    if (nk > 1) qsort(kids, (size_t)nk, sizeof *kids, docchild_cmp);
    for (int k = 0; k < nk; k++) {
        if (kids[k].doc) {
            docs_row(rows, n, max, kids[k].doc, NULL, kids[k].name, depth, kids[k].order);
            continue;
        }
        if (reveal && path_under(reveal->path, kids[k].path)) docs_set_expanded(kids[k].path, 1);
        docs_row(rows, n, max, NULL, kids[k].path, kids[k].name, depth, kids[k].order);
        if (all || docs_expanded(kids[k].path)) {
            docs_folder(rows, n, max, kids[k].path, depth + 1, all, reveal);
        }
    }
    for (int k = 0; names && k < nk; k++) free(names[k]);
    free(names);
    free(kids);
}

static void docs_rows_free(struct docrow *rows, int n)
{
    for (int i = 0; i < n; i++) {
        free(rows[i].path);
        rows[i].path = NULL;
    }
}

/* The list's rows: with all, every folder open (the documents' order for
 * Previous and Next Document). reveal: the folders of that document opened. */
static int docs_build(struct docrow *rows, int max, int all, struct doc *reveal)
{
    int n = 0;
    struct docchild *top = calloc((size_t)ndocs + 1, sizeof *top);
    char **roots = NULL;
    int nroots = docs_list_mode ? 0 : docs_roots(&roots), nt = 0;
    for (int i = 0; top && i < nroots; i++) {
        int order = ndocs;
        for (int k = 0; k < ndocs; k++) {
            if (docs[k]->path[0] && path_under(docs[k]->path, roots[i]) && k < order) order = k;
        }
        top[nt++] = (struct docchild){ NULL, roots[i], strcmp(roots[i], "/") == 0 ? "/" : last_part(roots[i]), order };
    }
    char dname[ndocs ? ndocs : 1][256];
    for (int i = 0; top && i < ndocs; i++) {
        if (!docs_list_mode && docs[i]->path[0]) continue;
        doc_name(docs[i], dname[i], sizeof dname[i]); /* List Mode, and documents without a file: documentName */
        top[nt++] = (struct docchild){ docs[i], NULL, dname[i], i };
    }
    if (nt > 1) qsort(top, (size_t)nt, sizeof *top, docchild_cmp);
    for (int k = 0; k < nt; k++) {
        if (top[k].doc) {
            docs_row(rows, &n, max, top[k].doc, NULL, top[k].name, 0, top[k].order);
            continue;
        }
        if (reveal && reveal->path[0] && path_under(reveal->path, top[k].path)) docs_set_expanded(top[k].path, 1);
        docs_row(rows, &n, max, NULL, top[k].path, top[k].name, 0, top[k].order);
        if (all || docs_expanded(top[k].path)) {
            docs_folder(rows, &n, max, top[k].path, 1, all, reveal);
        }
    }
    for (int i = 0; i < nroots; i++) free(roots[i]);
    free(roots);
    free(top);
    return n;
}

static struct doc *active_doc(void)
{
    struct tab *t = cur_tab();
    return t ? t->doc : NULL;
}

static void docs_refresh(void)
{
    struct doc *a = active_doc();
    docs_rows_free(docs_rows, ndocs_rows);
    ndocs_rows = docs_build(docs_rows, DOCS_MAX_ROWS, 0, a != docs_shown_active ? a : NULL);
    if (a != docs_shown_active) {
        docs_shown_active = a;
        int h = screen_rows() - 3 - docs_toolbar;
        for (int i = 0; i < ndocs_rows; i++) {
            if (docs_rows[i].doc && docs_rows[i].doc == a) {
                if (i < docs_top) docs_top = i; /* scrollTo */
                if (h > 0 && i >= docs_top + h) docs_top = i - h + 1;
            }
        }
    }
}

/* The shading of the history (KateFileTreeModel::updateBackgrounds): the
 * last ten documents viewed in the view shade, the last ten edited in the
 * edit shade blended in, the most recent strongest. */
static uint32_t docs_shade(struct doc *d, uint32_t base)
{
    if (!docs_shading) return base;
    enum { HISTORY = 10 };
    int view = 0, edit = 0, hc = 0, ec = 0;
    struct docui *u = ui_of(d);
    long last_view = 0;
    for (int s = 0; s < nspaces; s++) {
        for (int i = 0; i < spaces[s].ntabs; i++) {
            if (spaces[s].tabs[i].doc == d && spaces[s].tabs[i].used > last_view) last_view = spaces[s].tabs[i].used;
        }
    }
    for (int i = 0; i < ndocs; i++) {
        long lv = 0;
        for (int s = 0; s < nspaces; s++) {
            for (int k = 0; k < spaces[s].ntabs; k++) {
                if (spaces[s].tabs[k].doc == docs[i] && spaces[s].tabs[k].used > lv) lv = spaces[s].tabs[k].used;
            }
        }
        hc += lv > 0;
        ec += ui_of(docs[i])->edited > 0;
        view += lv > 0 && last_view > 0 && lv >= last_view;
        edit += ui_of(docs[i])->edited > 0 && u->edited > 0 && ui_of(docs[i])->edited >= u->edited;
    }
    hc = hc < HISTORY ? hc : HISTORY;
    ec = ec < HISTORY ? ec : HISTORY;
    if (!last_view || view > HISTORY) view = 0;
    if (!u->edited || edit > HISTORY) edit = 0;
    if (!view && !edit) return base;
    /* The view shade tints with Breeze's VisitedText, the edit shade with
     * its ActiveText (KColorUtils::tint at 0.5, approximated by a blend). */
    uint32_t shade = colour_mix(base, 0x9B59B6, 77), eshade = colour_mix(base, 0x3DAEE9, 77);
    if (edit > 0) {
        int v = hc - view, e = ec - edit + 1;
        e = e * e;
        int nn = v + e > 1 ? v + e : 1;
        uint32_t mixed = 0;
        for (int shift = 0; shift <= 16; shift += 8) {
            int sc = (int)(shade >> shift & 0xFF), ecc = (int)(eshade >> shift & 0xFF);
            mixed |= (uint32_t)((sc * v + ecc * e) / nn) << shift;
        }
        shade = mixed;
    }
    double t = hc ? (double)(hc - view + 1) / hc : 1;
    return colour_mix(base, shade, t >= 1 ? 255 : (int)(t * 255));
}

static void docs_draw(void)
{
    int rows = screen_rows(), y = 3;
    docs_refresh();
    if (docs_toolbar) {
        static const char *const buttons[] = { N_("New"), N_("Open"), N_("Save"), N_("Save As"), N_("Prev"), N_("Next") };
        int x = 1;
        for (int i = 0; i < 6; i++) {
            const char *btn = _(buttons[i]);
            int n = ui_cells(btn);
            if (x + n > SIDE_W - 1) break;
            put_text(x, y, SIDE_W, btn, C_FOCUS, C_WINDOW);
            ui_hit(x, y, n, 1, HIT_DOCBAR + i);
            x += n + 1;
        }
        y++;
    }
    int h = rows - y;
    if (docs_top > ndocs_rows - h) docs_top = ndocs_rows - h;
    if (docs_top < 0) docs_top = 0;
    ui_hit(0, y, SIDE_W, h, HIT_DOCROW + DOCS_EMPTY);
    struct doc *a = active_doc();
    for (int i = docs_top; i < ndocs_rows && i < docs_top + h; i++) {
        struct docrow *r = &docs_rows[i];
        int row = y + i - docs_top, on = r->doc && r->doc == a;
        uint32_t bg = on ? C_FOCUS : r->doc ? docs_shade(r->doc, C_WINDOW) : C_WINDOW;
        const char *mark = !r->doc ? (docs_expanded(r->path) ? "- " : "+ ") :
                           r->doc->disk != DISK_NONE ? "! " : doc_modified(r->doc) ? "* " : "  ";
        char line[400];
        snprintf(line, sizeof line, "%*s%s%s", 1 + 2 * r->depth, "", mark, r->name);
        screen_fill(0, row, SIDE_W, 1, bg);
        put_text(0, row, SIDE_W, line, on ? 0xFFFFFF : !r->doc ? C_FOCUS : C_WTEXT, bg);
        ui_hit(0, row, SIDE_W, 1, HIT_DOCROW + i);
    }
}

static void docs_forget(struct doc *d)
{
    if (docs_shown_active == d) docs_shown_active = NULL;
}

static void docs_wheel(int steps)
{
    docs_top += steps;
    if (docs_top < 0) docs_top = 0;
}

static void docs_activate(struct doc *d)
{
    if (!d) return;
    show_doc(active, d);
    bar.focus = FOCUS_VIEW;
    tree_focus = 0;
    follow_project();
}

/* Previous and Next Document: the documents in the list's order, round. */
static void docs_step(int dir)
{
    struct docrow *all = docs_scratch;
    int n = docs_build(all, DOCS_MAX_ROWS, 1, NULL), count = 0, at = -1;
    struct doc *order[DOCS_MAX_ROWS], *a = active_doc();
    for (int i = 0; i < n; i++) {
        if (all[i].doc) {
            if (all[i].doc == a) at = count;
            order[count++] = all[i].doc;
        }
    }
    docs_rows_free(all, n);
    if (!count) return;
    docs_activate(order[at < 0 ? 0 : (at + dir + count) % count]);
}

/* The documents under a folder row. */
static int docs_under(const struct docrow *r, struct doc **out)
{
    int n = 0;
    for (int i = 0; i < ndocs; i++) {
        if (r->doc ? docs[i] == r->doc : (docs[i]->path[0] && path_under(docs[i]->path, r->path))) out[n++] = docs[i];
    }
    return n;
}

static void docs_close_list(struct doc **list, int n)
{
    for (int i = 0; i < n; i++) {
        int there = 0;
        for (int k = 0; k < ndocs; k++) there |= docs[k] == list[i];
        if (there && !close_doc(list[i])) break;
    }
    follow_project();
}

enum { DOCS_OPEN, DOCS_COPY, DOCS_RELOAD, DOCS_CLOSE, DOCS_CLOSE_OTHER, DOCS_EXPAND, DOCS_COLLAPSE, DOCS_TREE,
       DOCS_LIST, DOCS_BY_NAME, DOCS_BY_PATH, DOCS_BY_OPENING };

static void docs_menu(int row, int col, int y)
{
    struct docrow *r = row >= 0 && row < ndocs_rows ? &docs_rows[row] : NULL;
    const char *items[16];
    char labels[16][64];
    int actions[16], n = 0;
#define DOCS_ITEM(label, action, checked) \
    (snprintf(labels[n], sizeof labels[n], "%c %s", (checked) ? '*' : ' ', label), actions[n] = action, \
     items[n] = labels[n], n++)
    if (r && r->doc && r->doc->path[0]) {
        DOCS_ITEM(_("Open..."), DOCS_OPEN, 0);
        DOCS_ITEM(_("Copy Location"), DOCS_COPY, 0);
        DOCS_ITEM(_("Reload"), DOCS_RELOAD, 0);
        DOCS_ITEM(_("Close"), DOCS_CLOSE, 0);
        DOCS_ITEM(_("Close Other"), DOCS_CLOSE_OTHER, 0);
    } else if (r && r->doc) {
        DOCS_ITEM(_("Close"), DOCS_CLOSE, 0);
    } else if (r) {
        DOCS_ITEM(_("Open..."), DOCS_OPEN, 0);
        DOCS_ITEM(_("Reload"), DOCS_RELOAD, 0);
        DOCS_ITEM(_("Close"), DOCS_CLOSE, 0);
        DOCS_ITEM(_("Expand Recursively"), DOCS_EXPAND, 0);
        DOCS_ITEM(_("Collapse Recursively"), DOCS_COLLAPSE, 0);
    }
    DOCS_ITEM(_("Tree Mode"), DOCS_TREE, !docs_list_mode);
    DOCS_ITEM(_("List Mode"), DOCS_LIST, docs_list_mode);
    DOCS_ITEM(_("Sort By Document Name"), DOCS_BY_NAME, docs_sort == DOCS_SORT_NAME);
    DOCS_ITEM(_("Sort By Document Path"), DOCS_BY_PATH, docs_sort == DOCS_SORT_PATH);
    DOCS_ITEM(_("Sort By Opening Order"), DOCS_BY_OPENING, docs_sort == DOCS_SORT_OPENING);
#undef DOCS_ITEM
    items[n] = NULL;
    int chosen = ui_menu(col, y, items);
    if (chosen < 0) return;
    struct doc *list[ndocs ? ndocs : 1];
    int count;
    switch (actions[chosen]) {
    case DOCS_OPEN: open_dialog(); break;
    case DOCS_COPY: clipboard_set(0, r->doc->path, strlen(r->doc->path)); break;
    case DOCS_RELOAD:
        count = docs_under(r, list);
        for (int i = 0; i < count; i++) {
            if (list[i]->path[0] && (!doc_modified(list[i]) || query_close(list[i]))) reload(list[i]);
        }
        break;
    case DOCS_CLOSE:
        count = docs_under(r, list);
        docs_close_list(list, count);
        break;
    case DOCS_CLOSE_OTHER: {
        /* The other documents of its folder (slotDocumentCloseOther). */
        struct docrow parent = { 0 };
        for (int i = row - 1; i >= 0 && !parent.path; i--) {
            if (!docs_rows[i].doc && docs_rows[i].depth < r->depth) parent = docs_rows[i];
        }
        count = 0;
        for (int i = 0; i < ndocs; i++) {
            if (docs[i] != r->doc && (!parent.path || (docs[i]->path[0] && path_under(docs[i]->path, parent.path)))) {
                list[count++] = docs[i];
            }
        }
        docs_close_list(list, count);
        break;
    }
    case DOCS_EXPAND:
    case DOCS_COLLAPSE:
        for (int i = row; i < ndocs_rows && (i == row || docs_rows[i].depth > r->depth); i++) {
            if (!docs_rows[i].doc) docs_set_expanded(docs_rows[i].path, actions[chosen] == DOCS_EXPAND);
        }
        if (actions[chosen] == DOCS_EXPAND) {
            /* Folders not shown yet: all those under it. */
            struct docrow *all = docs_scratch;
            int n_all = docs_build(all, DOCS_MAX_ROWS, 1, NULL);
            for (int i = 0; i < n_all; i++) {
                if (!all[i].doc && path_under(all[i].path, r->path)) docs_set_expanded(all[i].path, 1);
            }
            docs_rows_free(all, n_all);
        }
        break;
    case DOCS_TREE: docs_list_mode = 0; break;
    case DOCS_LIST: docs_list_mode = 1; break;
    case DOCS_BY_NAME: docs_sort = DOCS_SORT_NAME; break;
    case DOCS_BY_PATH: docs_sort = DOCS_SORT_PATH; break;
    case DOCS_BY_OPENING: docs_sort = DOCS_SORT_OPENING; break;
    }
}

static int docs_pointer(int id, const struct pointer_event *e)
{
    if (id >= HIT_DOCBAR && id < HIT_DOCBAR + 6 && e->button == BTN_LEFT) {
        struct tab *t = cur_tab();
        switch (id - HIT_DOCBAR) {
        case 0: new_document(); break;
        case 1: open_dialog(); break;
        case 2: if (t) save(t->doc); break;
        case 3: if (t) save_as(t->doc); break;
        case 4: docs_step(-1); break;
        case 5: docs_step(1); break;
        }
        return 1;
    }
    if (id < HIT_DOCROW || id > HIT_DOCROW + DOCS_EMPTY) return 0;
    int row = id - HIT_DOCROW;
    if (e->button == BTN_RIGHT) {
        docs_menu(row == DOCS_EMPTY ? -1 : row, e->col, e->row);
        return 1;
    }
    if (e->button != BTN_LEFT || row >= ndocs_rows) return e->button == BTN_LEFT;
    struct docrow *r = &docs_rows[row];
    if (r->doc) {
        docs_activate(r->doc);
    } else if (e->clicks == 2 || e->col == 1 + 2 * r->depth) {
        docs_set_expanded(r->path, !docs_expanded(r->path)); /* a folder: on its mark, or a double click */
    }
    return 1;
}

/*
 * The document switcher.
 */
static struct {
    int open, sel, n;
    int held;                /* opened with Ctrl held: its release switches */
    struct doc **list;
} doc_switcher;

static int switcher_open(void)
{
    return doc_switcher.open;
}

static int sw_by_use(const void *pa, const void *pb)
{
    long a = ui_of(*(struct doc *const *)pa)->raised, b = ui_of(*(struct doc *const *)pb)->raised;
    return a < b ? 1 : a > b ? -1 : 0;
}

/* The documents, the last raised (shown, or created) first. */
static void switcher_list(void)
{
    free(doc_switcher.list);
    doc_switcher.list = malloc((size_t)(ndocs ? ndocs : 1) * sizeof *doc_switcher.list);
    doc_switcher.n = doc_switcher.list ? ndocs : 0;
    for (int i = 0; i < doc_switcher.n; i++) doc_switcher.list[i] = docs[i];
    if (doc_switcher.n > 1) qsort(doc_switcher.list, (size_t)doc_switcher.n, sizeof *doc_switcher.list, sw_by_use);
}

/* walk(from, to): opening at from + step, then on, round. */
static void switcher_walk(int backward)
{
    int step = backward ? -1 : 1;
    if (!doc_switcher.open) {
        switcher_list();
        if (doc_switcher.n <= 1) {
            free(doc_switcher.list);
            doc_switcher.list = NULL;
            doc_switcher.n = 0;
            return;
        }
        int from = backward ? doc_switcher.n - 1 : 0;
        doc_switcher.sel = from + step >= 0 && from + step < doc_switcher.n ? from + step : 0;
        doc_switcher.open = 1;
        return;
    }
    int from = backward ? doc_switcher.n - 1 : 0, to = backward ? 0 : doc_switcher.n - 1, next = doc_switcher.sel + step;
    doc_switcher.sel = next == to + step ? from : next;
}

static void switcher_close(int activate)
{
    if (activate && doc_switcher.sel >= 0 && doc_switcher.sel < doc_switcher.n) docs_activate(doc_switcher.list[doc_switcher.sel]);
    free(doc_switcher.list);
    doc_switcher.list = NULL;
    doc_switcher.n = 0;
    doc_switcher.open = 0;
}

static void switcher_tick(void)
{
    if (doc_switcher.open && doc_switcher.held && !(keys_modifiers() & KEYS_CTRL)) switcher_close(1); /* Ctrl released */
}

/* From the View menu: no Ctrl to release; Enter or a click switches. */
static void switcher_menu(int backward)
{
    if (!doc_switcher.open) {
        switcher_walk(backward);
        doc_switcher.held = 0;
    }
}

static int switcher_key(int key, int mods)
{
    int ctrl = (mods & KEYS_CTRL) != 0, shift = (mods & KEYS_SHIFT) != 0, alt = (mods & KEYS_ALT) != 0;
    if (!doc_switcher.open) {
        if (ctrl && !alt && key == '\t') {
            switcher_walk(shift); /* Last Used Views, Last Used Views (Reverse) */
            doc_switcher.held = 1;
            return 1;
        }
        return 0;
    }
    if (!ctrl && doc_switcher.held) {
        switcher_close(1); /* Ctrl went up before this key: switched, then the key as ever */
        return 0;
    }
    if (key == 27) {
        switcher_close(0);
    } else if (key == '\n') {
        switcher_close(1);
    } else if (key == '\t') {
        switcher_walk(shift);
    } else if (key == K_UP || key == K_DOWN) {
        doc_switcher.sel += key == K_UP ? (doc_switcher.sel > 0 ? -1 : 0) : (doc_switcher.sel + 1 < doc_switcher.n ? 1 : 0);
    } else if (key == K_BOL || key == K_EOL) {
        doc_switcher.sel = key == K_BOL ? 0 : doc_switcher.n - 1;
    } else if (key == 'w' && ctrl && !shift) {
        /* Close View: the selected document; the list stays. */
        struct doc *d = doc_switcher.list[doc_switcher.sel];
        close_doc(d);
        int sel = doc_switcher.sel;
        switcher_list();
        doc_switcher.sel = sel < doc_switcher.n ? sel : doc_switcher.n - 1;
        if (doc_switcher.n == 0) switcher_close(0);
    } else if (key > 0 && key < 0x80 && isalnum(key)) {
        /* The next one whose name starts so (keyboardSearch), round. */
        for (int k = 1; k <= doc_switcher.n; k++) {
            int i = (doc_switcher.sel + k) % doc_switcher.n;
            char name[256];
            doc_name(doc_switcher.list[i], name, sizeof name);
            if (tolower((unsigned char)name[0]) == tolower(key)) {
                doc_switcher.sel = i;
                break;
            }
        }
    }
    return 1;
}

/* The common start of the documents' folders, not cut in a name. */
static size_t switcher_prefix(void)
{
    const char *first = NULL;
    size_t n = 0;
    int paths = 0;
    for (int i = 0; i < doc_switcher.n; i++) {
        const char *p = doc_switcher.list[i]->path;
        if (!p[0]) continue;
        paths++;
        if (!first) {
            first = p;
            n = strlen(p);
            continue;
        }
        size_t k = 0;
        while (k < n && p[k] == first[k]) k++;
        n = k;
    }
    if (paths < 2 || !first) return 0;
    while (n > 0 && first[n - 1] != '/') n--;
    return n <= 1 ? 0 : n;
}

/* A row's two columns (TabswitcherFilesModel): the folder after the common
 * prefix of all, and the document's name. */
static void switcher_texts(int i, size_t prefix, char *name, size_t nsize, char *path, size_t psize)
{
    struct doc *d = doc_switcher.list[i];
    doc_name(d, name, nsize);
    path[0] = '\0';
    if (d->path[0] && strlen(d->path) > prefix) {
        const char *base = strrchr(d->path, '/');
        int len = (int)(base - d->path) - (int)prefix;
        if (len > 0) snprintf(path, psize, "%.*s", len, d->path + prefix);
    }
}

static void switcher_draw(void)
{
    if (!doc_switcher.open) return;
    int cols = screen_cols(), rows = screen_rows(), wide = 0, wpath = 0;
    size_t prefix = switcher_prefix();
    char name[256], path[512];
    for (int i = 0; i < doc_switcher.n; i++) {
        switcher_texts(i, prefix, name, sizeof name, path, sizeof path);
        int n = text_cells(name), p = text_cells(path);
        wide = n > wide ? n : wide;
        wpath = p > wpath ? p : wpath;
    }
    int w = wpath + wide + 5, h = doc_switcher.n > 6 ? doc_switcher.n : 6;
    w = w > cols * 3 / 4 ? cols * 3 / 4 : w;
    h = h > rows * 3 / 4 ? rows * 3 / 4 : h;
    int x = (cols - w) / 2, y = (rows - h) / 2, top = doc_switcher.sel >= h ? doc_switcher.sel - h + 1 : 0;
    screen_fill(x, y, w, h, C_WINDOW);
    int pw = wpath < w - wide - 4 ? wpath : w - wide - 4;
    pw = pw < 0 ? 0 : pw;
    for (int r = 0; r < h && top + r < doc_switcher.n; r++) {
        int i = top + r, on = i == doc_switcher.sel;
        uint32_t bg = on ? C_FOCUS : C_WINDOW;
        switcher_texts(i, prefix, name, sizeof name, path, sizeof path);
        screen_fill(x, y + r, w, 1, bg);
        int pc = text_cells(path);
        put_text(x + 1 + (pw > pc ? pw - pc : 0), y + r, x + 1 + pw, path, on ? C_WINDOW : C_DIM, bg); /* right-aligned */
        put_text(x + 2 + pw, y + r, x + w - 1, name, on ? 0xFFFFFF : C_WTEXT, bg);
        ui_hit(x, y + r, w, 1, HIT_SWITCHER + i);
    }
}

static int switcher_pointer(int id, const struct pointer_event *e)
{
    if (!doc_switcher.open || (!e->button && !e->wheel)) return 0;
    if (e->button == BTN_LEFT && e->pressed && id >= HIT_SWITCHER && id < HIT_SWITCHER + doc_switcher.n) {
        doc_switcher.sel = id - HIT_SWITCHER; /* pressed: switched to */
        switcher_close(1);
    } else if (e->pressed && !(id >= HIT_SWITCHER && id < HIT_SWITCHER + doc_switcher.n)) {
        switcher_close(0); /* a popup: a press outside closes it */
    }
    return 1;
}
