/* Find in Files adapter. Include after edit-session-ui.inc, before actions.
 * Hooks: filesearch_menu(); filesearch_tick() once per step; while
 * filesearch_active() clamp event timeout to 50ms; filesearch_done() on exit. */
#ifndef EDIT_FILESEARCH_UI_INCLUDED
#define EDIT_FILESEARCH_UI_INCLUDED
#include "edit-filesearch.h"
#include <glib.h>

static struct {
    struct edit_filesearch *search;
    char *root, *pattern, *replace, *include, *exclude;
    int scope, mode, match_case, recursive, hidden, follow_links;
    size_t selected;
    int initialized;
} filesearch_ui;

static void filesearch_init(void)
{
    if (filesearch_ui.initialized) return;
    filesearch_ui.initialized = 1;
    filesearch_ui.recursive = 1;
    filesearch_ui.pattern = strdup(""); filesearch_ui.replace = strdup("");
    filesearch_ui.include = strdup("*"); filesearch_ui.exclude = strdup("");
    char folder[PATH_MAX] = "";
    struct tab *t = cur_tab();
    if (project && project->root[0]) snprintf(folder, sizeof folder, "%s", project->root);
    else if (t && t->doc->path[0]) {
        snprintf(folder, sizeof folder, "%s", t->doc->path);
        char *slash = strrchr(folder, '/'); if (slash) *slash = '\0';
    }
    /* Never silently choose '/' or a user's entire home as the scope. */
    filesearch_ui.root = strdup(folder);
}
static int filesearch_prompt(const char *label, char **value)
{
    struct field field = {0};
    if (*value) field_set(&field, *value, strlen(*value));
    int ok = prompt_line(label, &field);
    if (ok) {
        char *next = strndup(field.s, (size_t)field.len);
        if (!next) ok = 0; else { free(*value); *value = next; }
    }
    return ok;
}
static int filesearch_active(void)
{
    return filesearch_ui.search && !edit_filesearch_done(filesearch_ui.search);
}
static void filesearch_tick(void)
{
    if (!filesearch_ui.search || edit_filesearch_done(filesearch_ui.search)) return;
    int rc = edit_filesearch_read(filesearch_ui.search);
    if (edit_filesearch_done(filesearch_ui.search)) {
        char text[128];
        if (rc < 0) snprintf(text, sizeof text, _("Find in Files: %.*s"), utf8_fit(edit_filesearch_error(filesearch_ui.search), 48), edit_filesearch_error(filesearch_ui.search));
        else {
            size_t count = edit_filesearch_count(filesearch_ui.search);
            snprintf(text, sizeof text, P_("Find in Files: %zu match (Search menu: results)",
                                           "Find in Files: %zu matches (Search menu: results)", count), count);
        }
        say_toast(text);
    }
}
static int filesearch_start_ui(void)
{
    filesearch_init();
    /* Open Documents is a scope; Folder/Project use the same snapshots only
     * as overlays on their enumerated paths, never silently widening scope. */
    struct edit_search_source *sources = calloc(ndocs ? (size_t)ndocs : 1, sizeof *sources);
    size_t count = 0;
    if (!sources) { error_box(_("Not enough memory for open-document snapshots.")); return 0; }
    for (int i = 0; i < ndocs; i++) if (docs[i]->path[0]) {
        sources[count++] = (struct edit_search_source){docs[i]->path, buffer_text(&docs[i]->buf), doc_len(docs[i])};
    }
    const char *root = filesearch_ui.root ? filesearch_ui.root : "";
    if (filesearch_ui.scope == 1) {
        if (!project || !project->root[0]) { free(sources); error_box(_("There is no current project. Choose Folder or Open Documents.")); return 0; }
        root = project->root;
    }
    struct edit_filesearch_options options = {
        .root = root, .pattern = filesearch_ui.pattern, .include = filesearch_ui.include, .exclude = filesearch_ui.exclude,
        .mode = filesearch_ui.mode, .match_case = filesearch_ui.match_case, .recursive = filesearch_ui.recursive,
        .hidden = filesearch_ui.hidden, .follow_links = filesearch_ui.follow_links,
        .sources = filesearch_ui.scope == 2 ? sources : NULL, .nsources = filesearch_ui.scope == 2 ? count : 0,
        .overlays = filesearch_ui.scope == 2 ? NULL : sources, .noverlays = filesearch_ui.scope == 2 ? 0 : count
    };
    char error[256];
    struct edit_filesearch *next = edit_filesearch_start(&options, error, sizeof error);
    free(sources);
    if (!next) { error_box(error); return 0; }
    edit_filesearch_free(filesearch_ui.search); filesearch_ui.search = next; filesearch_ui.selected = 0;
    say_toast(_("Find in Files: searching in background"));
    return 1;
}
static struct doc *filesearch_open_doc(const char *path)
{
    for (int i = 0; i < ndocs; i++) if (!strcmp(docs[i]->path, path)) return docs[i];
    return NULL;
}
static void filesearch_open_match(struct edit_filematch *match)
{
    if (!match) return;
    struct doc *d = filesearch_open_doc(match->path);
    if (!d) d = open_file(match->path);
    if (!d) return;
    struct tab *t = show_doc(active, d);
    if (!t) return;
    t->anchor = min_z(match->start, doc_len(d)); t->cursor = min_z(match->end, doc_len(d));
    show_cursor(active, t, 1);
}
static void filesearch_replace_checked(void)
{
    struct edit_filesearch *search = filesearch_ui.search;
    if (!search || !edit_filesearch_done(search) || *edit_filesearch_error(search)) {
        error_box(_("Wait for a successful completed search before replacing checked matches.")); return;
    }
    size_t count = edit_filesearch_count(search), checked = 0;
    for (size_t i = 0; i < count; i++) if (edit_filesearch_match(search, i)->checked) checked++;
    if (!checked) { error_box(_("No checked matches.")); return; }
    if (!filesearch_prompt(_("Replace checked with:"), &filesearch_ui.replace)) return;
    char question[384]; snprintf(question, sizeof question,
        P_("Replace %zu checked match?\nOpen documents become unsaved undoable edits. Other files are saved atomically, preserving encoding and line endings. Changed snapshots are refused.",
           "Replace %zu checked matches?\nOpen documents become unsaved undoable edits. Other files are saved atomically, preserving encoding and line endings. Changed snapshots are refused.",
           checked), checked);
    const char *const buttons[] = {_("&Replace Checked"), _("&Cancel")};
    if (ask(_("Replace in Files"), question, buttons, 2, 1, 1) != 0) return;
    GHashTable *visited = g_hash_table_new(g_str_hash, g_str_equal);
    size_t changed = 0;
    for (size_t i = 0; i < count; i++) {
        struct edit_filematch *m = edit_filesearch_match(search, i);
        if (!m->checked || g_hash_table_contains(visited, m->path)) continue;
        g_hash_table_add(visited, m->path);
        struct doc *d = filesearch_open_doc(m->path);
        /* Files opened from results become buffer edits, never disk overwrites.
         * They must still represent the exact unmodified on-disk snapshot. */
        if ((m->from_buffer && !d) || (d && (d->readonly || (!m->from_buffer &&
            (doc_modified(d) || d->flags || memcmp(d->digest, m->digest, DIGEST_SIZE)))))) {
            error_box(_("The document was closed, changed, wrapped or made read-only. Search again; no overwrite was performed.")); break;
        }
        const char *text = m->from_buffer ? buffer_text(&d->buf) : NULL;
        size_t length = m->from_buffer ? doc_len(d) : 0;
        struct edit_filesearch_plan plan;
        char error[512];
        int e = edit_filesearch_prepare(search, m->path, filesearch_ui.replace, text, length, &plan, error, sizeof error);
        if (e) { error_box(error); break; }
        e = edit_filesearch_plan_validate(&plan, text, length);
        if (!e && d) {
            uint8_t normalized[DIGEST_SIZE];
            GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
            char header[40]; int hn = snprintf(header, sizeof header, "blob %zu", doc_len(d));
            g_checksum_update(sum, (const guchar *)header, hn + 1);
            g_checksum_update(sum, (const guchar *)buffer_text(&d->buf), (gssize)doc_len(d));
            gsize dn = DIGEST_SIZE; g_checksum_get_digest(sum, normalized, &dn); g_checksum_free(sum);
            if (memcmp(normalized, plan.text_digest, DIGEST_SIZE)) e = ESTALE;
        }
        if (!e && d) {
            struct replacement r = {0, doc_len(d), plan.text, plan.length};
            doc_safe_point(d);
            if (doc_replace_many(d, &r, 1) < 0) e = ENOMEM;
            doc_safe_point(d);
        } else if (!e) {
            struct stat st; char final[PATH_MAX]; uint8_t digest[DIGEST_SIZE];
            e = file_save(plan.path, plan.text, plan.length, &plan.format, &st, final, sizeof final, digest);
        }
        if (e) { snprintf(error, sizeof error, _("Replacement refused for %.*s: %s"), utf8_fit(m->path, 300), m->path, strerror(e)); error_box(error); edit_filesearch_plan_free(&plan); break; }
        changed += plan.replacements;
        edit_filesearch_plan_free(&plan);
        for (size_t j = i; j < count; j++) {
            struct edit_filematch *same = edit_filesearch_match(search, j);
            if (!strcmp(same->path, m->path)) same->checked = 0;
        }
    }
    g_hash_table_unref(visited);
    char text[96]; snprintf(text, sizeof text, P_("Replaced %zu match; search again", "Replaced %zu matches; search again", changed), changed); say_toast(text);
}
/* Unlike ui_menu, the results view must drain its worker while waiting. -2
 * asks its caller to rebuild the labels after progress, without losing focus. */
static int filesearch_result_menu(const char *const *items, int n, int *focus)
{
    if (*focus < 0 || *focus >= n) *focus = 0;
    for (;;) {
        draw(); ui_hits_clear();
        int x = 1, y = 3, width = screen_cols() - 2;
        int visible = screen_rows() - y - 1;
        if (visible < 1) return -1;
        int top = *focus >= visible ? *focus - visible + 1 : 0;
        screen_fill(x, y, width, visible, C_WINDOW);
        for (int row = 0; row < visible && top + row < n; row++) {
            int index = top + row; uint32_t bg = index == *focus ? C_FOCUS : C_WINDOW;
            screen_fill(x, y + row, width, 1, bg);
            put_text(x + 1, y + row, x + width - 1, items[index], index == *focus ? C_WINDOW : C_WTEXT, bg);
            ui_hit(x, y + row, width, 1, HIT_DIALOG + index);
        }
        screen_flush();
        size_t before = edit_filesearch_count(filesearch_ui.search);
        int active_before = filesearch_active();
        struct pollfd p = {edit_filesearch_fd(filesearch_ui.search), POLLIN, 0};
        int key = key_poll_many(&p, 1, active_before ? 50 : -1);
        filesearch_tick();
        if (key == 27) return -1;
        if (key == '\n') return *focus;
        if (key == K_UP) *focus = (*focus + n - 1) % n;
        else if (key == K_DOWN || key == '\t') *focus = (*focus + 1) % n;
        else if (key == K_BOL) *focus = 0;
        else if (key == K_EOL) *focus = n - 1;
        else if (key == K_PGUP) *focus = *focus > visible ? *focus - visible : 0;
        else if (key == K_PGDN) *focus = *focus + visible < n ? *focus + visible : n - 1;
        else if (key == K_POINTER) {
            const struct pointer_event *event = pointer_last();
            if (event->button == BTN_LEFT && event->pressed) {
                int id = ui_hit_at(event->col, event->row) - HIT_DIALOG;
                if (id >= 0 && id < n) return id;
            }
        }
        if (before != edit_filesearch_count(filesearch_ui.search) || active_before != filesearch_active()) return -2;
    }
}
static void filesearch_results_ui(void)
{
    struct edit_filesearch *search = filesearch_ui.search;
    if (!search) { error_box(_("Start a Find in Files search first.")); return; }
    /* Bounded pages keep scanning and refreshing during keyboard/mouse waits. */
    int focus = 1;
    for (;;) {
        filesearch_tick();
        size_t count = edit_filesearch_count(search), page = filesearch_ui.selected / 16, first = page * 16;
        char labels[24][768]; const char *items[25]; int n = 0;
        const char *status = *edit_filesearch_error(search) ? edit_filesearch_error(search) :
                             *edit_filesearch_warning(search) ? edit_filesearch_warning(search) :
                             filesearch_active() ? _("Searching...") : _("Results");
        snprintf(labels[n], sizeof labels[n], P_("%.*s: %zu match", "%.*s: %zu matches", count), utf8_fit(status, 650), status, count); items[n] = labels[n]; n++;
        for (size_t i = first; i < count && i < first + 16; i++) {
            struct edit_filematch *m = edit_filesearch_match(search, i);
            snprintf(labels[n], sizeof labels[n], "[%c] %.*s:%zu:%zu  %.*s", m->checked ? 'x' : ' ', utf8_fit(m->path, 300), m->path, m->line, m->column, utf8_fit(m->preview, 300), m->preview);
            items[n] = labels[n]; n++;
        }
        int prev = n; items[n++] = _("Previous page");
        int next = n; items[n++] = _("Next page");
        int toggle = n; items[n++] = _("Toggle all checked");
        int replace = n; items[n++] = _("Replace checked...");
        int close = n; items[n++] = _("Close"); items[n] = NULL;
        int picked = filesearch_result_menu(items, n, &focus);
        if (picked == -2) continue;
        if (picked < 0 || picked == close || picked == 0) return;
        if (picked == prev) { filesearch_ui.selected = first >= 16 ? first - 16 : 0; continue; }
        if (picked == next) { if (first + 16 < count) filesearch_ui.selected = first + 16; continue; }
        if (picked == toggle) {
            int any = 0; for (size_t i = 0; i < count; i++) if (!edit_filesearch_match(search, i)->checked) any = 1;
            for (size_t i = 0; i < count; i++) edit_filesearch_match(search, i)->checked = any;
            continue;
        }
        if (picked == replace) { filesearch_replace_checked(); continue; }
        size_t index = first + (size_t)picked - 1;
        if (picked > 0 && index < count && index < first + 16) {
            struct edit_filematch *m = edit_filesearch_match(search, index); filesearch_ui.selected = index;
            const char *const actions[] = {_("Open result"), _("Toggle checked"), _("Cancel"), NULL};
            int action = ui_menu(2, 3, actions);
            if (action == 0) { filesearch_open_match(m); return; }
            if (action == 1) m->checked = !m->checked;
        }
    }
}
static void filesearch_menu(void)
{
    filesearch_init();
    for (;;) {
        filesearch_tick();
        char text[14][512]; const char *items[15];
        snprintf(text[0], sizeof text[0], _("Find: %.*s"), utf8_fit(filesearch_ui.pattern, 450), filesearch_ui.pattern);
        snprintf(text[1], sizeof text[1], _("Scope: %s"), filesearch_ui.scope == 2 ? _("Open Documents") : filesearch_ui.scope == 1 ? _("Current Project") : _("Folder"));
        snprintf(text[2], sizeof text[2], _("Folder: %.*s"), utf8_fit(filesearch_ui.root, 450), filesearch_ui.root);
        snprintf(text[3], sizeof text[3], _("Include: %.*s"), utf8_fit(filesearch_ui.include, 450), filesearch_ui.include);
        snprintf(text[4], sizeof text[4], _("Exclude: %.*s"), utf8_fit(filesearch_ui.exclude, 450), filesearch_ui.exclude);
        snprintf(text[5], sizeof text[5], _("Mode: %s"), _(mode_names[filesearch_ui.mode]));
        snprintf(text[6], sizeof text[6], "[%c] %s", filesearch_ui.match_case ? 'x' : ' ', _("Match case"));
        snprintf(text[7], sizeof text[7], "[%c] %s", filesearch_ui.recursive ? 'x' : ' ', _("Recursive"));
        snprintf(text[8], sizeof text[8], "[%c] %s", filesearch_ui.hidden ? 'x' : ' ', _("Hidden files"));
        snprintf(text[9], sizeof text[9], "[%c] %s", filesearch_ui.follow_links ? 'x' : ' ', _("Follow symbolic links"));
        snprintf(text[10], sizeof text[10], "%s", _("Start search"));
        snprintf(text[11], sizeof text[11], filesearch_active() ? _("Results (%zu) - searching") : _("Results (%zu)"),
                 edit_filesearch_count(filesearch_ui.search));
        snprintf(text[12], sizeof text[12], "%s", _("Cancel running search")); snprintf(text[13], sizeof text[13], "%s", _("Close"));
        for (int i = 0; i < 14; i++) items[i] = text[i];
        items[14] = NULL;
        switch (ui_menu(1, 3, items)) {
        case 0: filesearch_prompt(_("Find in Files:"), &filesearch_ui.pattern); break;
        case 1: filesearch_ui.scope = (filesearch_ui.scope + 1) % 3; break;
        case 2: filesearch_prompt(_("Search folder:"), &filesearch_ui.root); break;
        case 3: filesearch_prompt(_("Include globs (; separated):"), &filesearch_ui.include); break;
        case 4: filesearch_prompt(_("Exclude globs (; separated):"), &filesearch_ui.exclude); break;
        case 5: filesearch_ui.mode = (filesearch_ui.mode + 1) % 4; break;
        case 6: filesearch_ui.match_case ^= 1; break;
        case 7: filesearch_ui.recursive ^= 1; break;
        case 8: filesearch_ui.hidden ^= 1; break;
        case 9: filesearch_ui.follow_links ^= 1; break;
        case 10: if (filesearch_start_ui()) return; break;
        case 11: filesearch_results_ui(); return;
        case 12: edit_filesearch_free(filesearch_ui.search); filesearch_ui.search = NULL; say_toast(_("Find in Files cancelled")); return;
        default: return;
        }
    }
}
static void filesearch_done(void)
{
    edit_filesearch_free(filesearch_ui.search);
    free(filesearch_ui.root); free(filesearch_ui.pattern); free(filesearch_ui.replace);
    free(filesearch_ui.include); free(filesearch_ui.exclude);
    memset(&filesearch_ui, 0, sizeof filesearch_ui);
}
#endif
