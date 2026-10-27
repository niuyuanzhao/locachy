/* Session actions and the live editor adapter. Included after edit's document,
 * tab, dialog and split helpers, before edit-actions.inc. The storage module
 * deliberately knows nothing about these screen/editor globals. */
#include "edit-session.h"
#include <glib.h>

static char session_name[EDIT_SESSION_MAX_NAME + 1] = "default";
static int session_enabled, session_blocked, session_last_error;
static double session_next;
static char session_digest[65];

static void session_error(const char *operation, const char *fmt, int error)
{
    char message[768];
    snprintf(message, sizeof message, fmt, session_name, strerror(error));
    say("edit: session %s: %s: %s", session_name, operation, strerror(error));
    error_box(message);
}

static int session_capture(struct edit_session_state *out)
{
    memset(out, 0, sizeof *out);
    if (ndocs < 0 || (unsigned)ndocs > EDIT_SESSION_MAX_DOCUMENTS || nspaces < 1 ||
        (unsigned)nspaces > EDIT_SESSION_MAX_SPACES) return EFBIG;
    out->documents = calloc(ndocs ? (size_t)ndocs : 1, sizeof *out->documents);
    out->spaces = calloc((size_t)nspaces, sizeof *out->spaces);
    out->layout = edit_layout_encode(&view_layout);
    if (!out->documents || !out->spaces || !out->layout) goto memory;
    out->ndocuments = (size_t)ndocs;
    out->nspaces = (size_t)nspaces;
    out->active = active;
    out->sidebar = sidebar;
    out->sidebar_width = sidebar_width;
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i];
        struct edit_session_document *to = &out->documents[i];
        to->length = doc_len(d);
        to->format = d->format;
        to->modified = doc_modified(d);
        to->readonly = d->readonly;
        if (d->path[0] && !(to->path = strdup(d->path))) goto memory;
        if (to->modified && d->path[0] && d->st.st_ino) file_digest_hex(d->digest, to->base);
        if ((to->modified || !d->path[0]) && !(to->text = buffer_dup(&d->buf, 0, to->length))) goto memory;
    }
    for (int s = 0; s < nspaces; s++) {
        struct edit_session_space *to = &out->spaces[s];
        to->ntabs = (size_t)spaces[s].ntabs;
        to->current = spaces[s].cur;
        to->tabs = calloc(to->ntabs ? to->ntabs : 1, sizeof *to->tabs);
        if (!to->tabs) goto memory;
        for (int j = 0; j < spaces[s].ntabs; j++) {
            struct tab *t = &spaces[s].tabs[j];
            int document = -1;
            for (int i = 0; i < ndocs; i++) if (docs[i] == t->doc) { document = i; break; }
            if (document < 0) { edit_session_free(out); return EINVAL; }
            to->tabs[j] = (struct edit_session_view){
                .document = document, .cursor = t->cursor, .anchor = t->anchor, .top = t->top,
                .left = t->left, .sub = t->sub, .wrap = t->wrap, .numbers = t->numbers, .font_px = t->font_px
            };
        }
    }
    return 0;
memory:
    edit_session_free(out);
    return ENOMEM;
}

/* Build every replacement document, tab and splitter before touching the live
 * editor. A corrupt/missing/unreadable target never discards the current work. */
static int session_apply(const struct edit_session_state *state)
{
    if (state->nspaces < 1 || state->nspaces > MAX_SPACES || state->ndocuments > INT_MAX) return EFBIG;
    struct doc **prepared = calloc(state->ndocuments ? state->ndocuments : 1, sizeof *prepared);
    struct space next[MAX_SPACES] = {0};
    struct edit_layout next_layout = {0};
    int e = ENOMEM;
    if (!prepared) return e;
    if (edit_layout_decode(&next_layout, state->layout, (int)state->nspaces) < 0) { e = EINVAL; goto fail; }
    for (size_t i = 0; i < state->ndocuments; i++) {
        const struct edit_session_document *from = &state->documents[i];
        struct doc *d = calloc(1, sizeof *d);
        struct docui *u = calloc(1, sizeof *u);
        if (!d || !u) { free(d); free(u); goto fail; }
        if (doc_init(d) < 0) { free(u); free(d); goto fail; }
        prepared[i] = d;
        d->ui = u;
        u->number = -1;
        u->limit = LINE_LIMIT;
        u->reopen_encoding = from->format.encoding + 1;
        if (from->path) snprintf(d->path, sizeof d->path, "%s", from->path);
        if (from->text) {
            if (doc_set_text(d, from->text, from->length) < 0) goto fail;
            if (d->path[0] && from->modified) {
                /* The recovered edits are relative to the bytes recorded at
                 * checkpoint time, never to whatever the disk holds now. */
                char target[PATH_MAX], now_hex[2 * DIGEST_SIZE + 1] = "";
                uint8_t now_digest[DIGEST_SIZE];
                struct stat st;
                file_as_user(1);
                file_resolve(d->path, target, sizeof target);
                int there = stat(target, &st) == 0;
                file_as_user(0);
                if (there && !file_digest(target, now_digest)) file_digest_hex(now_digest, now_hex);
                if (from->base[0]) {
                    for (int k = 0; k < DIGEST_SIZE; k++) {
                        unsigned byte;
                        sscanf(from->base + 2 * k, "%2x", &byte);
                        d->digest[k] = (uint8_t)byte;
                    }
                }
                if (there) d->st = st;
                if (!there) d->disk = from->base[0] ? DISK_DELETED : DISK_NONE;
                else if (!from->base[0]) d->disk = DISK_CREATED;
                else if (strcmp(now_hex, from->base)) d->disk = DISK_MODIFIED;
            }
            d->format = from->format;
            d->saved = from->modified ? -1 : 0;
            doc_detect_syntax(d);
        } else {
            if (!d->path[0] || load(d) < 0) { e = EIO; goto fail; }
            d->format = from->format;
        }
        d->readonly |= from->readonly;
        d->moved = moved;
    }
    for (size_t s = 0; s < state->nspaces; s++) {
        const struct edit_session_space *from = &state->spaces[s];
        next[s].tabs = calloc(from->ntabs ? from->ntabs : 1, sizeof *next[s].tabs);
        if (!next[s].tabs) goto fail;
        next[s].ntabs = next[s].size = (int)from->ntabs;
        next[s].cur = from->current;
        next[s].share = 1000 / (int)state->nspaces;
        for (size_t j = 0; j < from->ntabs; j++) {
            const struct edit_session_view *v = &from->tabs[j];
            struct doc *d = prepared[v->document];
            size_t length = doc_len(d);
            struct tab *t = &next[s].tabs[j];
            *t = (struct tab){ .doc = d, .cursor = min_z(v->cursor, length), .anchor = min_z(v->anchor, length),
                .top = line_start(d, line_of(d, min_z(v->top, length))), .left = v->left, .sub = v->sub,
                .wrap = v->wrap, .numbers = v->numbers, .font_px = v->font_px, .goal = -1, .used = ++use_clock,
                .scrollbar = view_scrollbar_default, .minimap = view_minimap_default, .marks = view_marks_default,
                .icons = view_icons_default };
        }
    }
    while (ndocs) free_doc(docs[0]);
    free(docs);
    for (int s = 0; s < MAX_SPACES; s++) {
        for (int i = 0; i < spaces[s].ntabs; i++) {
            mc_free(&spaces[s].tabs[i]);
            fold_release(&spaces[s].tabs[i]);
        }
        free(spaces[s].tabs);
    }
    edit_layout_free(&view_layout);
    projects_free();
    project = NULL;
    docs = prepared;
    ndocs = (int)state->ndocuments;
    memcpy(spaces, next, sizeof spaces);
    nspaces = (int)state->nspaces;
    active = state->active;
    sidebar = state->sidebar;
    sidebar_width = state->sidebar_width;
    view_layout = next_layout;
    split_drag = NULL;
    sidebar_drag = tree_focus = tree_sel = tree_top = 0;
    drag.mode = DRAG_NONE;
    primary_pending = 0;
    bar_kind = BAR_NONE;
    bar.focus = FOCUS_VIEW;
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i];
        update_name(d);
        watch(d);
        if (doc_modified(d))
            /* TRANSLATORS: %1$s is the document's path or "Untitled", %2$s is the session name. */
            post(d, NOTE_INFO, 1, 0, _("Unsaved changes to %s were restored from session '%s'."),
                 display_path(d), session_name);
        if (d->disk != DISK_NONE) {
            /* As check_disk's prompt: Save then asks Save Nevertheless. */
            d->disk_shown = d->disk;
            struct note *n = post(d, NOTE_WARNING, 1, 0, d->disk == DISK_DELETED ?
                _("The file '%s' was deleted or moved on disk after these unsaved changes were recorded.") :
                _("The file '%s' was changed on disk after these unsaved changes were recorded."), d->path);
            /* No one-click Reload here: it would drop the recovered edits.
             * File > Reload asks Save/Discard first; Save asks Save
             * Nevertheless while d->disk is set. */
            if (d->disk == DISK_DELETED) {
                n->actions[n->nactions++] = ACT_CLOSE_FILE;
                n->actions[n->nactions++] = ACT_SAVE_AS;
            }
            n->actions[n->nactions++] = ACT_IGNORE;
        }
    }
    follow_project();
    return 0;
fail:
    for (size_t i = 0; i < state->ndocuments; i++) if (prepared[i]) {
        struct docui *u = ui_of(prepared[i]);
        free(u->hl);
        free(u);
        doc_free(prepared[i]);
        free(prepared[i]);
    }
    free(prepared);
    for (int s = 0; s < MAX_SPACES; s++) free(next[s].tabs);
    edit_layout_free(&next_layout);
    return e;
}

/* Hash the live state without allocating another full set of buffer copies.
 * This also catches merged edits, mouse scroll, splitter drags and view-only
 * changes, not just key presses or the undo group's count. */
static void session_hash_layout(GChecksum *sum, const struct edit_split *split)
{
    if (!split) {
        const int missing = -2;
        g_checksum_update(sum, (const guchar *)&missing, sizeof missing);
        return;
    }
    g_checksum_update(sum, (const guchar *)&split->view, sizeof split->view);
    if (split->view >= 0) return;
    g_checksum_update(sum, (const guchar *)&split->vertical, sizeof split->vertical);
    g_checksum_update(sum, (const guchar *)&split->ratio, sizeof split->ratio);
    session_hash_layout(sum, split->first);
    session_hash_layout(sum, split->second);
}

static void session_fingerprint(char digest[65])
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
#define HASH(value) g_checksum_update(sum, (const guchar *)&(value), sizeof(value))
    HASH(ndocs); HASH(nspaces); HASH(active); HASH(sidebar); HASH(sidebar_width);
    session_hash_layout(sum, view_layout.root);
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i];
        size_t length = doc_len(d);
        int modified = doc_modified(d);
        g_checksum_update(sum, (const guchar *)d->path, strlen(d->path) + 1);
        HASH(length); HASH(modified); HASH(d->readonly); HASH(d->format.encoding); HASH(d->format.bom); HASH(d->format.eol);
        if (modified || !d->path[0]) for (size_t at = 0; at < length;) {
            size_t n;
            const char *part = buffer_chunk(&d->buf, at, &n);
            g_checksum_update(sum, (const guchar *)part, n);
            at += n;
        }
    }
    for (int s = 0; s < nspaces; s++) {
        HASH(spaces[s].cur); HASH(spaces[s].ntabs);
        for (int j = 0; j < spaces[s].ntabs; j++) {
            struct tab *t = &spaces[s].tabs[j];
            HASH(t->doc); HASH(t->cursor); HASH(t->anchor); HASH(t->top); HASH(t->left); HASH(t->sub);
            HASH(t->wrap); HASH(t->numbers); HASH(t->font_px);
        }
    }
#undef HASH
    snprintf(digest, 65, "%s", g_checksum_get_string(sum));
    g_checksum_free(sum);
}

static int session_checkpoint(int clean, int report)
{
    if (session_blocked) {
        if (report) session_error("save (recovery is protected)",
            _("Could not save (recovery is protected) session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), EBUSY);
        return EBUSY;
    }
    struct edit_session_state state = {0};
    int e = session_capture(&state);
    if (!e) e = edit_session_save(session_name, &state, clean);
    edit_session_free(&state);
    if (!e) { session_fingerprint(session_digest); session_last_error = 0; }
    else if (report || e != session_last_error) {
        session_last_error = e;
        session_error("save",
            _("Could not save session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
    }
    return e;
}

static void session_tick(void)
{
    if (!session_enabled || session_blocked || now() < session_next) return;
    session_next = now() + 2.0;
    char digest[65];
    session_fingerprint(digest);
    if (strcmp(digest, session_digest)) (void)session_checkpoint(0, 0);
}

static int session_timeout(int timeout)
{
    if (!session_enabled || session_blocked) return timeout;
    int due = (int)((session_next - now()) * 1000);
    if (due < 0) due = 0;
    return timeout < 0 || timeout > due ? due : timeout;
}

static void session_startup(void)
{
    struct edit_session_state state = {0};
    char *selected = NULL;
    int e = edit_session_selected(&selected);
    if (!e) snprintf(session_name, sizeof session_name, "%s", selected);
    free(selected);
    if (e != 0 && e != ENOENT) {
        session_enabled = session_blocked = 1;
        session_error("read selected",
            _("Could not read selected session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return;
    }
    e = edit_session_load(session_name, &state);
    session_enabled = 1;
    session_next = now() + 2.0;
    if (e != 0 && e != ENOENT) {
        session_blocked = 1;
        session_error("load",
            _("Could not load session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return;
    }
    if (!e && state.recovered) {
        const char *const buttons[] = { _("&Restore"), _("&Keep Recovery and Start Empty") };
        if (ask(_("Recover Session"), _("The editor did not finish its last session cleanly.\n"
                "Restore its documents, unsaved changes and views?\n"
                "Starting empty keeps the recovery checkpoint protected until another session is chosen."),
                buttons, 2, 0, 1) != 0) {
            session_blocked = 1;
            edit_session_free(&state);
            return;
        }
    }
    if (!e) {
        e = session_apply(&state);
        if (e) {
            session_blocked = 1;
            session_error("restore",
                _("Could not restore session '%s': %s.\n"
                  "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        } else if (state.recovered) say_toast(_("Recovered unsaved documents and views"));
    }
    edit_session_free(&state);
    if (!e || e == ENOENT) (void)session_checkpoint(0, 1);
}

/* A clean session retains modified buffers, as Kate's session recovery does;
 * closing an individual document still uses its existing Save/Discard dialog. */
static int session_quit(void)
{
    if (!session_enabled) return query_quit();
    if (session_blocked) {
        const char *const buttons[] = { _("&Quit Without Replacing Recovery"), _("&Cancel") };
        return ask(_("Protected Recovery"), _("This session's previous recovery is protected.\n"
                   "Use Save Session As to preserve current work in another session before quitting."),
                   buttons, 2, 1, 1) == 0;
    }
    return session_checkpoint(1, 1) == 0;
}

/* Paginate because ui_menu itself has no scrolling and names may exceed the
 * screen's height. The caller always receives an owned name or NULL. */
static char *session_choose(const char *title)
{
    char **names = NULL;
    size_t count = 0, page = 0;
    int e = edit_session_list(&names, &count);
    if (e) {
        session_error("list",
            _("Could not list session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return NULL;
    }
    if (!count) { say_toast(_("No saved sessions")); edit_session_list_free(names, count); return NULL; }
    size_t per = screen_rows() > 8 ? (size_t)screen_rows() - 6 : 2;
    if (per > 18) per = 18;
    char *result = NULL;
    for (;;) {
        const char *lines[22];
        char text[18][EDIT_SESSION_MAX_NAME + 8];
        size_t base = page * per, shown = min_z(per, count - base), n = 0;
        lines[n++] = title;
        for (size_t i = 0; i < shown; i++) {
            snprintf(text[i], sizeof text[i], "%c %s", !strcmp(names[base + i], session_name) ? '*' : ' ', names[base + i]);
            lines[n++] = text[i];
        }
        int previous = -1, next = -1;
        if (page) { previous = (int)n; lines[n++] = _("Previous page"); }
        if (base + shown < count) { next = (int)n; lines[n++] = _("Next page"); }
        lines[n] = NULL;
        int selected = ui_menu(2, 3, lines);
        if (selected < 0) break;
        if (selected == previous) { page--; continue; }
        if (selected == next) { page++; continue; }
        if (selected > 0 && (size_t)selected <= shown) { result = strdup(names[base + (size_t)selected - 1]); break; }
    }
    edit_session_list_free(names, count);
    return result;
}

static int session_unused_name(char name[EDIT_SESSION_MAX_NAME + 1])
{
    struct field field = {0};
    if (!prompt_line(_("Session name:"), &field)) return 0;
    if (field.len <= 0 || (size_t)field.len > EDIT_SESSION_MAX_NAME || (size_t)field.len != strlen(field.s)) {
        error_box(_("A session name must contain 1 to 128 UTF-8 bytes."));
        return 0;
    }
    memcpy(name, field.s, field.len + 1);
    char **names = NULL;
    size_t count = 0;
    int e = edit_session_list(&names, &count), exists = 0;
    for (size_t i = 0; !e && i < count; i++) if (!strcmp(names[i], name)) exists = 1;
    edit_session_list_free(names, count);
    if (e) {
        session_error("list",
            _("Could not list session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return 0;
    }
    if (exists) { error_box(_("A session with that name already exists. Switch to it or choose another name.")); return 0; }
    return 1;
}

static void session_save_as(void)
{
    char name[EDIT_SESSION_MAX_NAME + 1];
    if (!session_unused_name(name)) return;
    struct edit_session_state state = {0};
    int e = session_capture(&state);
    if (!e) e = edit_session_create(name, &state, 0);
    edit_session_free(&state);
    if (e) {
        session_error("create",
            _("Could not create session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return;
    }
    if (!session_blocked && session_enabled && session_checkpoint(1, 1)) return;
    snprintf(session_name, sizeof session_name, "%s", name);
    session_enabled = 1;
    session_blocked = session_last_error = 0;
    session_fingerprint(session_digest);
    session_next = now() + 2.0;
    e = edit_session_select(session_name);
    if (e) session_error("remember selected",
        _("Could not remember selected session '%s': %s.\n"
          "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
    say_toast(_("Session saved under the new name"));
}

static void session_new(void)
{
    char name[EDIT_SESSION_MAX_NAME + 1];
    if (!session_unused_name(name)) return;
    if (session_blocked) {
        error_box(_("The previous recovery is protected. Use Save Session As to preserve current work first."));
        return;
    }
    struct edit_session_space empty = { .current = -1 };
    /* The new session's window as this one's: its tool views stay as they are. */
    struct edit_session_state state = { .spaces = &empty, .nspaces = 1, .sidebar = sidebar, .sidebar_width = SIDE_DEFAULT,
                                        .layout = "0" };
    int e = session_checkpoint(1, 1);
    if (!e) e = edit_session_create(name, &state, 0);
    if (!e) e = session_apply(&state);
    if (e) {
        session_error("create",
            _("Could not create session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        return;
    }
    snprintf(session_name, sizeof session_name, "%s", name);
    session_fingerprint(session_digest);
    session_next = now() + 2.0;
    e = edit_session_select(session_name);
    if (e) session_error("remember selected",
        _("Could not remember selected session '%s': %s.\n"
          "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
    say_toast(_("New session"));
}

static void session_switch(void)
{
    char *name = session_choose(_("Switch session"));
    if (!name) return;
    if (!strcmp(name, session_name)) { free(name); return; }
    if (session_blocked) {
        error_box(_("The previous recovery is protected. Use Save Session As to preserve current work first."));
        free(name);
        return;
    }
    struct edit_session_state target = {0};
    int e = edit_session_load(name, &target);
    if (!e && target.recovered) {
        const char *const buttons[] = { _("&Restore"), _("&Cancel") };
        if (ask(_("Recover Session"), _("This session has an unclean checkpoint. Restore all its unsaved documents?"),
                buttons, 2, 0, 1) != 0) { edit_session_free(&target); free(name); return; }
    }
    if (!e) e = session_checkpoint(1, 1);
    if (!e) e = session_apply(&target);
    edit_session_free(&target);
    if (e) session_error("switch",
        _("Could not switch session '%s': %s.\n"
          "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
    else {
        snprintf(session_name, sizeof session_name, "%s", name);
        session_next = now() + 2.0;
        (void)session_checkpoint(0, 1);
        e = edit_session_select(session_name);
        if (e) session_error("remember selected",
            _("Could not remember selected session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        say_toast(_("Session switched"));
    }
    free(name);
}

static void session_remove_ui(void)
{
    char *name = session_choose(_("Delete saved session"));
    if (!name) return;
    if (!strcmp(name, session_name)) {
        error_box(_("The active session cannot be deleted. Switch to another session first."));
        free(name);
        return;
    }
    const char *const buttons[] = { _("&Delete Session"), _("&Cancel") };
    char text[512];
    snprintf(text, sizeof text, _("Delete session '%s', including its unsaved recovery buffers?\n"
             "This does not delete any document files."), name);
    if (ask(_("Delete Session"), text, buttons, 2, 1, 1) == 0) {
        int e = edit_session_remove(name);
        if (e) session_error("delete",
            _("Could not delete session '%s': %s.\n"
              "The existing checkpoint has been kept. Use another session name to continue without replacing it."), e);
        else say_toast(_("Session deleted"));
    }
    free(name);
}

static void session_menu(void)
{
    const char *const actions[] = {
        _("New Session..."), _("Switch Session..."), _("Save Session"), _("Save Session As..."), _("Delete Session..."), NULL
    };
    switch (ui_menu(2, 2, actions)) {
    case 0: session_new(); break;
    case 1: session_switch(); break;
    case 2: if (!session_checkpoint(0, 1)) say_toast(_("Session saved")); break;
    case 3: session_save_as(); break;
    case 4: session_remove_ui(); break;
    }
}
