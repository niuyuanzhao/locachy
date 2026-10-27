/* SPDX-License-Identifier: GPL-3.0-or-later
 * Language server UI/controller. Included immediately before edit-actions.inc.
 * Hooks in edit.c: language_changed at the end of moved; language_closed before
 * freeing a document; language_saved after a successful save; language_tick at
 * the start of step; cap input polling to 25ms while language_active; invoke
 * language_key before global_or_view_key; language_done before program cleanup.
 * All results come from the configured real stdio server. No heuristic engine.
 */
#include "edit-lsp.h"
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <ftw.h>
#include <pwd.h>

struct language_server {
    char *key, *language, *root;
    EdLsp *client;
    int failed_notice;
};
struct language_document {
    struct doc *doc;
    struct language_server *server;
    char *uri;
    int64_t version;
    EdLspEvent *diagnostics;
    int saving;
    char *snapshot;
};
struct language_request_ui {
    struct language_server *server;
    uint64_t id;
    struct doc *doc;
    size_t cursor, anchor;
    int automatic, pointer_hover;
    EdLspMethod method;
};
static GPtrArray *language_servers, *language_documents, *language_requests;
static JsonNode *language_settings;
static int language_initialized, language_busy, language_presenting, language_auto_completion = 1;
static int language_save_resume;
static int language_auto_signature = 1, language_auto_hover = 1, language_format_save;
static int language_show_diagnostics = 1, language_format_typing;
static const char *language_pick_help;
static int language_pick_help_requested;
static double language_hover_due;
static struct doc *language_hover_doc;
static size_t language_hover_offset;
static struct doc *language_trigger_doc;
static char *language_trigger;
static void language_tick(void);
static void language_changed(struct doc *d);
static void language_closed(struct doc *d);
static void language_saved(struct doc *d);
static void language_done(void);
static int language_key(int key, int mods);
static void language_menu(void);

static JsonNode *language_member(JsonNode *n, const char *key)
{
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_object_get_member(json_node_get_object(n), key) : NULL;
}
static const char *language_string(JsonNode *n)
{
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
static JsonNode *language_parse(const char *s)
{
    JsonParser *p = json_parser_new(); JsonNode *n = NULL;
    if (json_parser_load_from_data(p, s, -1, NULL)) n = json_node_copy(json_parser_get_root(p));
    g_object_unref(p); return n;
}
static char *language_home(void)
{
    struct passwd *pw = getpwuid(getuid() == 0 ? EDIT_USER : getuid());
    return g_strdup(pw && pw->pw_dir ? pw->pw_dir : g_get_home_dir());
}
static char *language_config_path(void)
{
    char *home = language_home(), *path = g_build_filename(home, ".config", "locachy", "edit-lsp.json", NULL);
    g_free(home); return path;
}
static void language_init(void)
{
    if (language_initialized) return;
    language_initialized = 1;
    language_servers = g_ptr_array_new(); language_documents = g_ptr_array_new(); language_requests = g_ptr_array_new_with_free_func(g_free);
    char *path = language_config_path(), *text = NULL;
    file_as_user(1); gboolean loaded = g_file_get_contents(path, &text, NULL, NULL); file_as_user(0);
    if (loaded) language_settings = language_parse(text);
    if (!language_settings || !JSON_NODE_HOLDS_OBJECT(language_settings)) {
        if (language_settings) json_node_free(language_settings);
        language_settings = language_parse("{\"servers\":{}}");
    }
    g_free(path); g_free(text);
    JsonNode *options = language_member(language_settings, "options");
#define LOAD_OPTION(key, variable) do { JsonNode *v = language_member(options, key); if (v && json_node_get_value_type(v) == G_TYPE_BOOLEAN) variable = json_node_get_boolean(v); } while (0)
    LOAD_OPTION("completion", language_auto_completion); LOAD_OPTION("signature", language_auto_signature);
    LOAD_OPTION("hover", language_auto_hover); LOAD_OPTION("formatOnSave", language_format_save);
    LOAD_OPTION("diagnostics", language_show_diagnostics); LOAD_OPTION("formatOnTyping", language_format_typing);
#undef LOAD_OPTION
}
static void language_write_settings(void)
{
    JsonObject *root = json_node_get_object(language_settings), *options = json_object_new();
    json_object_set_boolean_member(options, "completion", language_auto_completion);
    json_object_set_boolean_member(options, "signature", language_auto_signature);
    json_object_set_boolean_member(options, "hover", language_auto_hover);
    json_object_set_boolean_member(options, "formatOnSave", language_format_save);
    json_object_set_boolean_member(options, "diagnostics", language_show_diagnostics);
    json_object_set_boolean_member(options, "formatOnTyping", language_format_typing);
    json_object_set_object_member(root, "options", options);
    char *path = language_config_path(), *dir = g_path_get_dirname(path), *text = json_to_string(language_settings, TRUE);
    GError *error = NULL; file_as_user(1);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0 && g_file_set_contents(path, text, -1, &error);
    file_as_user(0);
    if (!ok && cur_tab()) post(cur_tab()->doc, NOTE_ERROR, 0, 5, _("Cannot save language settings: %s"), error ? error->message : strerror(errno));
    g_clear_error(&error); g_free(path); g_free(dir); g_free(text);
}
static const char *language_id(struct doc *d)
{
    const char *name = syntax_name(d->lang), *suffix = strrchr(d->path, '.');
    if (!strcmp(name, "C")) return "c";
    if (!strcmp(name, "C++")) return "cpp";
    if (!strcmp(name, "Rust")) return "rust";
    if (!strcmp(name, "Python")) return "python";
    if (!strcmp(name, "Bash") || !strcmp(name, "Shell")) return "bash";
    if (!strcmp(name, "JavaScript")) return "javascript";
    if (!strcmp(name, "TypeScript")) return "typescript";
    if (!strcmp(name, "Go")) return "go";
    if (!strcmp(name, "Lua")) return "lua";
    if (!strcmp(name, "JSON")) return "json";
    if (!strcmp(name, "YAML")) return "yaml";
    if (suffix) {
        static const char *const extensions[][2] = {{".c", "c"}, {".h", "c"}, {".cpp", "cpp"}, {".cc", "cpp"}, {".hpp", "cpp"}, {".rs", "rust"}, {".py", "python"}, {".sh", "bash"}, {".js", "javascript"}, {".jsx", "javascriptreact"}, {".ts", "typescript"}, {".tsx", "typescriptreact"}, {".go", "go"}, {".lua", "lua"}, {".json", "json"}, {".yaml", "yaml"}, {".yml", "yaml"}};
        for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; i++) if (!strcmp(suffix, extensions[i][0])) return extensions[i][1];
    }
    return "text";
}
static const char *language_server_type(const char *id)
{
    if (!strcmp(id, "cpp")) return "c";
    if (!strcmp(id, "typescript") || !strcmp(id, "typescriptreact") || !strcmp(id, "javascriptreact")) return "javascript";
    return id;
}
static const char *language_default_command(const char *type)
{
    static const char *const defaults[][2] = {{"c", "clangd --log=error --background-index"}, {"rust", "rust-analyzer"}, {"python", "pylsp"}, {"bash", "bash-language-server start"}, {"javascript", "typescript-language-server --stdio"}, {"go", "gopls"}, {"lua", "lua-language-server"}, {"json", "vscode-json-language-server --stdio"}, {"yaml", "yaml-language-server --stdio"}};
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) if (!strcmp(type, defaults[i][0])) return defaults[i][1];
    return "";
}
static char **language_command(const char *type)
{
    JsonNode *config = language_member(language_member(language_settings, "servers"), type);
    JsonNode *command = language_member(config, "command");
    if (command && JSON_NODE_HOLDS_ARRAY(command)) {
        JsonArray *a = json_node_get_array(command); guint n = json_array_get_length(a); char **argv = g_new0(char *, n + 1);
        for (guint i = 0; i < n; i++) argv[i] = g_strdup(language_string(json_array_get_element(a, i)));
        return argv;
    }
    const char *text = *language_string(command) ? language_string(command) : language_default_command(type);
    char **argv = NULL; if (*text) g_shell_parse_argv(text, NULL, &argv, NULL); return argv;
}
static struct language_document *language_binding(struct doc *d)
{
    if (!language_documents) return NULL;
    for (guint i = 0; i < language_documents->len; i++) {
        struct language_document *b = g_ptr_array_index(language_documents, i); if (b->doc == d) return b;
    }
    return NULL;
}
static struct language_document *language_by_uri(struct language_server *s, const char *uri)
{
    for (guint i = 0; language_documents && i < language_documents->len; i++) {
        struct language_document *b = g_ptr_array_index(language_documents, i); if (b->server == s && !g_strcmp0(b->uri, uri)) return b;
    }
    return NULL;
}
static char *language_root(struct doc *d, JsonNode *config)
{
    const char *configured = language_string(language_member(config, "root"));
    if (*configured && g_path_is_absolute(configured)) return g_strdup(configured);
    struct project *p = project_for(d->path);
    char *root = p ? g_strdup(p->root) : g_path_get_dirname(d->path);
    if (*configured) { char *next = g_build_filename(root, configured, NULL); g_free(root); root = next; }
    return root;
}
static struct language_document *language_attach(struct doc *d, int explicit_request)
{
    language_init(); struct language_document *b = language_binding(d); if (b) return b;
    if (!d->path[0]) { if (explicit_request) post(d, NOTE_INFO, 0, 4, _("Save this document before starting its language server.")); return NULL; }
    const char *id = language_id(d), *type = language_server_type(id);
    JsonNode *config = language_member(language_member(language_settings, "servers"), type);
    char **argv = language_command(type);
    if (!argv || !argv[0] || !*argv[0]) { g_strfreev(argv); if (explicit_request) post(d, NOTE_INFO, 0, 5, _("No language server configured for %s. Use Configure Server Command."), id); return NULL; }
    char *root = language_root(d, config), *key = g_strdup_printf("%s:%s", type, root);
    struct language_server *s = NULL;
    for (guint i = 0; i < language_servers->len; i++) {
        struct language_server *candidate = g_ptr_array_index(language_servers, i); if (!strcmp(candidate->key, key)) { s = candidate; break; }
    }
    if (!s) {
        s = g_new0(struct language_server, 1); s->key = g_strdup(key); s->language = g_strdup(type); s->root = g_strdup(root);
        char *binary; file_as_user(1); binary = g_find_program_in_path(argv[0]); file_as_user(0);
        if (binary) {
            char *root_uri = g_filename_to_uri(root, NULL, NULL), *settings = NULL, *initialization = NULL;
            JsonNode *v = language_member(config, "settings"); if (v) settings = json_to_string(v, FALSE);
            v = language_member(config, "initializationOptions"); if (v) initialization = json_to_string(v, FALSE);
            char **spawn = argv;
            char *home = language_home();
            /* The framebuffer program may run as root. Servers are user code,
             * so permanently drop all IDs and every supplementary group (the
             * user's input group would give them the keyboard) first. */
            if (getuid() == 0) {
                guint argc = g_strv_length(argv); spawn = g_new0(char *, argc + 10);
                spawn[0] = g_strdup("/usr/bin/setpriv"); spawn[1] = g_strdup("--reuid=1000"); spawn[2] = g_strdup("--regid=1000"); spawn[3] = g_strdup("--clear-groups"); spawn[4] = g_strdup("--");
                spawn[5] = g_strdup("/usr/bin/env"); spawn[6] = g_strdup("-u"); spawn[7] = g_strdup("XDG_RUNTIME_DIR"); spawn[8] = g_strdup_printf("HOME=%s", home);
                for (guint i = 0; i < argc; i++) spawn[i + 9] = g_strdup(argv[i]);
            }
            EdLspConfig c = {.argv = (const char *const *)spawn, .cwd = root, .root_uri = root_uri, .settings_json = settings, .initialization_options_json = initialization};
            GError *error = NULL; s->client = ed_lsp_new(&c, &error);
            if (!s->client) post(d, NOTE_WARNING, 0, 5, _("Cannot start %s: %s"), argv[0], error ? error->message : _("unknown error"));
            g_clear_error(&error); if (spawn != argv) g_strfreev(spawn);
            g_free(root_uri); g_free(settings); g_free(initialization); g_free(binary); g_free(home);
        } else if (explicit_request) post(d, NOTE_INFO, 0, 5, _("Language server '%s' is not installed. Configure an existing server; no server is installed automatically."), argv[0]);
        g_ptr_array_add(language_servers, s);
    }
    g_strfreev(argv); g_free(root); g_free(key);
    if (!s->client) { if (explicit_request) post(d, NOTE_INFO, 0, 5, _("Language server unavailable for %s. Configure a command or restart after installing it."), id); return NULL; }
    if (ed_lsp_state(s->client) == ED_LSP_FAILED || ed_lsp_state(s->client) == ED_LSP_EXITED) { if (explicit_request) post(d, NOTE_WARNING, 0, 5, _("Language server stopped. Use Restart Current Server.")); return NULL; }
    char *text = buffer_dup(&d->buf, 0, doc_len(d));
    if (!text || strlen(text) != doc_len(d) || !g_utf8_validate(text, -1, NULL)) { free(text); if (explicit_request) post(d, NOTE_WARNING, 0, 5, _("Language server requires valid UTF-8 text without NUL bytes.")); return NULL; }
    b = g_new0(struct language_document, 1); b->doc = d; b->server = s; b->uri = g_filename_to_uri(d->path, NULL, NULL); b->version = 1;
    GError *error = NULL;
    if (!b->uri || !ed_lsp_open(s->client, b->uri, id, text, b->version, &error)) {
        if (explicit_request) post(d, NOTE_WARNING, 0, 5, _("Cannot open language document: %s"), error ? error->message : _("invalid URI"));
        g_clear_error(&error); g_free(b->uri); g_free(b); free(text); return NULL;
    }
    b->snapshot = text; g_ptr_array_add(language_documents, b); return b;
}
static void language_closed(struct doc *d)
{
    if (!language_documents) return;
    for (guint i = 0; i < language_documents->len; i++) {
        struct language_document *b = g_ptr_array_index(language_documents, i); if (b->doc != d) continue;
        ed_lsp_close(b->server->client, b->uri, NULL); ed_lsp_event_free(b->diagnostics); g_free(b->uri); free(b->snapshot); g_free(b); g_ptr_array_remove_index(language_documents, i); break;
    }
    for (guint i = 0; language_requests && i < language_requests->len;) {
        struct language_request_ui *r = g_ptr_array_index(language_requests, i); if (r->doc == d) { ed_lsp_cancel(r->server->client, r->id); g_ptr_array_remove_index(language_requests, i); } else i++;
    }
    if (language_hover_doc == d) language_hover_doc = NULL;
    if (language_trigger_doc == d) language_trigger_doc = NULL;
}
static void language_changed(struct doc *d)
{
    if (language_busy) return;
    struct language_document *b = language_binding(d); if (!b) return;
    const char *old = ed_lsp_document(b->server->client, b->uri, NULL);
    char *text = buffer_dup(&d->buf, 0, doc_len(d));
    if (!old || !text || strlen(text) != doc_len(d) || !g_utf8_validate(text, -1, NULL)) { free(text); language_closed(d); return; }
    size_t old_len = strlen(old), new_len = doc_len(d), start = 0, tail = 0;
    while (start < old_len && start < new_len && old[start] == text[start]) start++;
    while (start && (((unsigned char)text[start] & 0xc0) == 0x80)) start--;
    while (tail < old_len - start && tail < new_len - start && old[old_len - tail - 1] == text[new_len - tail - 1]) tail++;
    while (tail && (((unsigned char)text[new_len - tail] & 0xc0) == 0x80)) tail--;
    if (start == old_len && start == new_len) { free(text); return; }
    char *replacement = g_strndup(text + start, new_len - start - tail);
    if (ed_lsp_change(b->server->client, b->uri, start, old_len - tail, replacement, ++b->version, NULL)) {
        ed_lsp_event_free(b->diagnostics); b->diagnostics = NULL;
        g_free(language_trigger); language_trigger = g_strdup(replacement); language_trigger_doc = d;
    }
    free(b->snapshot); b->snapshot = text;
    g_free(replacement); language_hover_doc = NULL;
}
static int language_active(void)
{
    return language_servers && language_servers->len > 0;
}
static uint64_t language_send(struct language_document *b, EdLspRequest *request, int automatic)
{
    if (!b || ed_lsp_state(b->server->client) != ED_LSP_READY) {
        if (b && !automatic) post(b->doc, NOTE_INFO, 0, 3, _("Language server is initializing; try again shortly."));
        return 0;
    }
    for (guint i = 0; automatic && i < language_requests->len;) {
        struct language_request_ui *old = g_ptr_array_index(language_requests, i);
        if (old->server == b->server && old->doc == b->doc && old->automatic && old->method == request->method) { ed_lsp_cancel(old->server->client, old->id); g_ptr_array_remove_index(language_requests, i); } else i++;
    }
    struct tab *t = cur_tab(); request->uri = b->uri;
    if (t && t->doc == b->doc) { request->offset = t->cursor; if (request->method == ED_LSP_CODE_ACTION || request->method == ED_LSP_RANGE_FORMAT) sel_range(t, &request->offset, &request->end_offset); }
    GError *error = NULL; uint64_t id = ed_lsp_request(b->server->client, request, &error);
    if (!id) { if (!automatic) post(b->doc, NOTE_WARNING, 0, 4, "%s", error ? error->message : _("Language request failed")); g_clear_error(&error); return 0; }
    struct language_request_ui *r = g_new0(struct language_request_ui, 1); r->server = b->server; r->id = id; r->doc = b->doc;
    r->cursor = t && t->doc == b->doc ? t->cursor : 0; r->anchor = t && t->doc == b->doc ? t->anchor : 0; r->automatic = automatic; r->method = request->method;
    g_ptr_array_add(language_requests, r); return id;
}

/* A keyboard/pointer selectable, scrollable real-result list. Timed input keeps
 * every server drained while the list is open. No result count is truncated. */
static int language_pick(const char *title, GPtrArray *labels)
{
    if (!labels || !labels->len) return -1;
    int selected = 0, top = 0; language_pick_help_requested = 0;
    for (;;) {
        draw(); int width = screen_cols() - 6, height = screen_rows() - 8;
        if (height < 1 || width < 8) return -1;
        if (selected < top) top = selected;
        if (selected >= top + height) top = selected - height + 1;
        screen_fill(3, 3, width, height + 2, C_WINDOW); screen_fill(3, 3, width, 1, C_FOCUS);
        put_text(4, 3, width + 2, title, C_WINDOW, C_FOCUS);
        for (int i = 0; i < height && top + i < (int)labels->len; i++) {
            uint32_t bg = top + i == selected ? C_FOCUS : C_WINDOW;
            screen_fill(3, 4 + i, width, 1, bg); put_text(4, 4 + i, width + 2, g_ptr_array_index(labels, top + i), C_WTEXT, bg);
        }
        /* TRANSLATORS: Keep key names (Enter, Esc, Up/Down/PgUp/PgDn) as is. */
        put_text(4, 4 + height, width + 2, language_pick_help ? language_pick_help : _("Enter: choose   Esc: cancel   Up/Down/PgUp/PgDn: select"), C_WTEXT, C_WINDOW); screen_flush();
        int key = key_poll_timeout(-1, 25), mods = keys_modifiers(); language_tick();
        if (key == 27) return -1;
        if (key == K_F1 && language_pick_help) { language_pick_help_requested = 1; return selected; }
        if (key == '\n' || key == '\t') return selected;
        if (key == K_UP) selected = selected > 0 ? selected - 1 : (int)labels->len - 1;
        else if (key == K_DOWN) selected = (selected + 1) % (int)labels->len;
        else if (key == K_PGUP) selected = selected > height ? selected - height : 0;
        else if (key == K_PGDN) selected = MIN(selected + height, (int)labels->len - 1);
        else if (key == K_BOL) selected = 0;
        else if (key == K_EOL) selected = (int)labels->len - 1;
        else if (key == K_POINTER) {
            const struct pointer_event *p = pointer_last();
            if (p->button == BTN_LEFT && p->pressed && p->row >= 4 && p->row < 4 + height && p->col >= 3 && p->col < 3 + width) {
                int at = top + p->row - 4; if (at < (int)labels->len) { if (at == selected) return at; selected = at; }
            }
        } else if (key == K_CTRL_SPACE && !(mods & KEYS_ALT)) { ime_cycle_mode(); }
    }
}
static void language_text(const char *title, const char *text)
{
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free); char **parts = g_strsplit(text ? text : "", "\n", -1);
    int columns = MAX(12, screen_cols() - 10);
    for (guint i = 0; parts[i]; i++) {
        const char *p = parts[i]; if (!*p) g_ptr_array_add(lines, g_strdup(" "));
        while (*p) {
            const char *end = p; int cells = 0;
            while (*end && cells < columns) { gunichar cp = g_utf8_get_char(end); int n = screen_char_cells(cp); if (cells + n > columns) break; cells += n; end = g_utf8_next_char(end); }
            if (end == p) end = g_utf8_next_char(p);
            g_ptr_array_add(lines, g_strndup(p, end - p)); p = end;
        }
    }
    language_pick(title, lines); g_strfreev(parts); g_ptr_array_unref(lines);
}
static struct doc *language_open_uri(const char *uri)
{
    char *host = NULL, *path = g_filename_from_uri(uri, &host, NULL);
    if (!path || (host && *host && strcmp(host, "localhost"))) { g_free(host); g_free(path); return NULL; }
    struct doc *d = open_file(path); g_free(host); g_free(path); return d;
}
static void language_jump(const char *uri, EdLspRange range)
{
    struct doc *d = language_open_uri(uri); if (!d) return;
    size_t start, end; char *text = buffer_dup(&d->buf, 0, doc_len(d));
    if (!text) return;
    if (!ed_lsp_position_utf8(text, range.start, &start) || !ed_lsp_position_utf8(text, range.end, &end)) { free(text); post(d, NOTE_WARNING, 0, 4, _("The server location no longer matches this document.")); return; }
    free(text);
    struct tab *t = show_doc(active, d); if (t) { t->cursor = start; t->anchor = end; show_cursor(active, t, 1); }
}

/* Workspace edits. Kate's client applies the text edits of `changes` and of
 * `documentChanges` (applyWorkspaceEdit, one transaction and so one undo step
 * per document) and ignores CreateFile/RenameFile/DeleteFile ("resourceOperations
 * not supported for now", parseWorkSpaceEdit). Those follow the LSP 3.17
 * specification here: in the order sent, as the user (file_as_user), refused
 * where the user may not. A file that an operation replaces or deletes is first
 * moved aside in its folder (".NAME.edit-lsp-XXXXXXXX") and removed only when
 * the whole edit succeeded; if a later change fails, everything done is undone
 * in reverse order and the server is told applied=false ("undo" failure
 * handling). Text edits are staged and applied last, each document's as one
 * undo step; open documents follow renames, and documents of deleted files are
 * asked about (query_close) and closed when the edit succeeded. */
struct language_staged { struct doc *doc; char *before, *after; size_t cursor, anchor; int applied; };
static void language_stage_free(gpointer pointer)
{
    struct language_staged *p = pointer; g_free(p->before); g_free(p->after); g_free(p);
}
struct language_op {
    EdLspEditKind kind;
    char *path, *old_path;      /* rename: old_path -> path */
    char *backup;               /* what was at path, moved aside */
    int made, backup_dir;       /* the operation happened; the backup is a folder */
    struct stat created;        /* create: the file written */
    GPtrArray *followed;        /* rename: the open documents that moved with it */
    struct doc *reloaded;       /* an unmodified document read again from the new file */
};
static void language_op_free(gpointer pointer)
{
    struct language_op *op = pointer;
    g_free(op->path); g_free(op->old_path); g_free(op->backup); if (op->followed) g_ptr_array_unref(op->followed); g_free(op);
}
struct language_edit {
    struct language_server *server;
    struct doc *origin;
    GPtrArray *staged;          /* struct language_staged* */
    GPtrArray *ops;             /* struct language_op*, in the order done */
    GPtrArray *closing;         /* struct doc*: closed once the edit succeeded */
    GPtrArray *opened;          /* struct doc*: opened for it; closed again if it fails */
};
static char language_failure[768];

static struct doc *language_alive(struct doc *d)
{
    for (int i = 0; d && i < ndocs; i++) if (docs[i] == d) return d;
    return NULL;
}
static void language_fail(struct doc *origin, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void language_fail(struct doc *origin, int kind, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(language_failure, sizeof language_failure, fmt, ap); va_end(ap);
    struct doc *note = language_alive(origin) ? origin : cur_tab() ? cur_tab()->doc : NULL;
    if (note) post(note, kind, 0, 6, "%s", language_failure);
}
static char *language_path(const char *uri)
{
    char *host = NULL, *path = uri && g_utf8_validate(uri, -1, NULL) ? g_filename_from_uri(uri, &host, NULL) : NULL;
    if (path && (!g_path_is_absolute(path) || (host && *host && strcmp(host, "localhost")) || strlen(path) >= PATH_MAX)) g_clear_pointer(&path, g_free);
    g_free(host); return path;
}
/* The name as rename and unlink see it: the folder's links resolved, the last
 * component as it is (it is what moves, even if it is a link). */
static void language_canon(const char *path, char *out, size_t size)
{
    char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path), real[PATH_MAX];
    file_as_user(1); int found = realpath(dir, real) != NULL; file_as_user(0);
    if (!found || snprintf(out, size, "%s/%s", strcmp(real, "/") ? real : "", base) >= (int)size) snprintf(out, size, "%s", path);
    g_free(dir); g_free(base);
}
static int language_under(const char *path, const char *dir)
{
    size_t n = strlen(dir); return n && !strncmp(path, dir, n) && path[n] == '/';
}
static int language_closing(struct language_edit *x, struct doc *d)
{
    for (guint i = 0; i < x->closing->len; i++) if (g_ptr_array_index(x->closing, i) == d) return 1;
    return 0;
}
/* The open document of a file, not one that this edit closes. */
static struct doc *language_doc_at(struct language_edit *x, const char *path)
{
    char canon[PATH_MAX]; language_canon(path, canon, sizeof canon);
    for (int i = 0; i < ndocs; i++) {
        if (docs[i]->path[0] && (!strcmp(docs[i]->path, path) || !strcmp(docs[i]->path, canon)) && !language_closing(x, docs[i])) return docs[i];
    }
    return NULL;
}
static int language_lstat(const char *path, struct stat *st)
{
    file_as_user(1); int r = lstat(path, st), e = errno; file_as_user(0); errno = e; return r;
}
/* rename(2) as the user that never replaces what is at to. */
static int language_rename(const char *from, const char *to)
{
    file_as_user(1);
    int r = renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE), e = errno;
    if (r < 0 && (e == EINVAL || e == ENOSYS)) {
        /* A file system without RENAME_NOREPLACE: looked at, then renamed. */
        struct stat st;
        if (lstat(to, &st) == 0) e = EEXIST;
        else if (errno != ENOENT) e = errno;
        else { r = rename(from, to); e = errno; }
    }
    file_as_user(0); errno = e; return r < 0 ? -1 : 0;
}
/* path moved aside to a new name beside it: that name, or NULL and errno. */
static char *language_move_aside(const char *path)
{
    char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path), *aside = NULL; int e = EEXIST;
    if (strlen(base) > 200) base[200] = '\0';
    for (int i = 0; i < 64 && e == EEXIST; i++) {
        g_free(aside); aside = g_strdup_printf("%s/.%s.edit-lsp-%08x", strcmp(dir, "/") ? dir : "", base, g_random_int());
        e = language_rename(path, aside) == 0 ? 0 : errno;
    }
    g_free(dir); g_free(base);
    if (e) { g_free(aside); errno = e; return NULL; }
    return aside;
}
static int language_remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw; return remove(path) < 0 ? -1 : 0;
}
/* close_doc after its query_close has been answered: every view, then it. */
static void language_close_now(struct doc *d)
{
    for (int s = nspaces - 1; s >= 0; s--) { int i = tab_index(s, d); if (i >= 0) drop_tab(s, i); }
    free_doc(d);
    for (int s = nspaces - 1; s >= 0; s--) space_emptied(s);
}
/* A document's file renamed or moved back: its name, watch and syntax follow;
 * the language binding follows at the next tick (didClose, then didOpen). */
static void language_move_doc(struct doc *d, const char *path)
{
    unwatch(d); snprintf(d->path, sizeof d->path, "%s", path);
    update_name(d); watch(d); doc_detect_syntax(d); ui_of(d)->project = NULL;
}
/* A TextDocumentEdit staged against its document's text: opened if it is not,
 * and must then exist; its version, when given, must be the one sent. */
static int language_stage_text(struct language_edit *x, EdLspDocumentEdit *change, GString *summary)
{
    char *path = language_path(change->uri);
    if (!path) { language_fail(x->origin, NOTE_ERROR, _("Cannot edit %s: not a local file."), change->uri); return 0; }
    struct doc *d = language_doc_at(x, path);
    if (!d) {
        struct stat st;
        if (language_lstat(path, &st) < 0) { language_fail(x->origin, NOTE_ERROR, _("Cannot edit %s: %s."), path, errno == ENOENT ? _("it does not exist") : strerror(errno)); g_free(path); return 0; }
        /* new_doc puts a new document last; it was alive before open_file did. */
        struct doc *last = ndocs ? docs[ndocs - 1] : NULL; d = open_file(path);
        if (d && d != last && ndocs && docs[ndocs - 1] == d) g_ptr_array_add(x->opened, d);
    }
    g_free(path);
    if (!d || d->readonly || d->flags & (FILE_BROKEN | FILE_WRAPPED)) { language_fail(x->origin, NOTE_ERROR, _("Cannot edit %s: unavailable or read-only."), change->uri); return 0; }
    struct language_document *binding = language_binding(d);
    if (change->version >= 0 && (!binding || strcmp(binding->uri, change->uri) || binding->version != change->version)) {
        language_fail(x->origin, NOTE_WARNING, _("Stale edit for %s; no changes applied."), d->path); return 0;
    }
    struct language_staged *p = NULL;
    for (guint j = 0; j < x->staged->len; j++) { struct language_staged *candidate = g_ptr_array_index(x->staged, j); if (candidate->doc == d) p = candidate; }
    if (!p) {
        p = g_new0(struct language_staged, 1); p->doc = d; p->before = buffer_dup(&d->buf, 0, doc_len(d)); p->after = g_strdup(p->before);
        struct tab *t = cur_tab(); if (t && t->doc == d) { p->cursor = t->cursor; p->anchor = t->anchor; }
        g_ptr_array_add(x->staged, p); if (summary) g_string_append_printf(summary, "%s\n", d->path);
    }
    GError *error = NULL; char *next = ed_lsp_apply_text_edits(p->after, binding && change->version >= 0 ? binding->version : -1, change, &error);
    if (!next) { language_fail(x->origin, NOTE_ERROR, _("Invalid workspace edit: %s"), error ? error->message : _("invalid edit")); g_clear_error(&error); return 0; }
    g_free(p->after); p->after = next; return 1;
}
/* An unmodified document of a file that an operation replaces: refused if it
 * has unsaved changes. */
static int language_replaceable(struct language_edit *x, const char *path, const char *unsaved_msg, struct doc **out)
{
    *out = language_doc_at(x, path);
    if (*out && doc_modified(*out)) { language_fail(x->origin, NOTE_ERROR, unsaved_msg, path); return 0; }
    return 1;
}
static struct language_op *language_op_new(struct language_edit *x, EdLspEditKind kind, const char *path)
{
    struct language_op *op = g_new0(struct language_op, 1); op->kind = kind; op->path = g_strdup(path); g_ptr_array_add(x->ops, op); return op;
}
/* CreateFile: an empty file through the editor's atomic save (file_save). */
static int language_create(struct language_edit *x, EdLspDocumentEdit *change, const char *path)
{
    struct stat st; struct doc *d = NULL;
    if (language_lstat(path, &st) == 0) {
        if (!change->overwrite) {
            if (change->ignore_if_exists) return 1;
            language_fail(x->origin, NOTE_ERROR, _("Cannot create %s: it already exists."), path); return 0;
        }
        if (S_ISDIR(st.st_mode)) { language_fail(x->origin, NOTE_ERROR, _("Cannot overwrite %s: it is a folder."), path); return 0; }
        if (!language_replaceable(x, path, _("Cannot overwrite %s: it is open with unsaved changes."), &d)) return 0;
        struct language_op *op = language_op_new(x, ED_LSP_EDIT_CREATE, path);
        if (!(op->backup = language_move_aside(path))) { language_fail(x->origin, NOTE_ERROR, _("Cannot overwrite %s: %s."), path, strerror(errno)); return 0; }
    } else if (errno != ENOENT) {
        language_fail(x->origin, NOTE_ERROR, _("Cannot create %s: %s."), path, strerror(errno)); return 0;
    } else {
        if (!language_replaceable(x, path, _("Cannot create %s: it is open with unsaved changes."), &d)) return 0;
        language_op_new(x, ED_LSP_EDIT_CREATE, path);
    }
    struct language_op *op = g_ptr_array_index(x->ops, x->ops->len - 1);
    struct file_format format = { ENC_UTF8, 0, EOL_LF }; char final[PATH_MAX]; uint8_t digest[DIGEST_SIZE];
    int err = file_save(path, "", 0, &format, &op->created, final, sizeof final, digest);
    if (err) { language_fail(x->origin, NOTE_ERROR, _("Cannot create %s: %s."), path, strerror(err)); return 0; }
    op->made = 1;
    if (d) { op->reloaded = d; reload(d); }
    return 1;
}
/* RenameFile: open documents of the file, or of the files in the folder, follow. */
static int language_rename_file(struct language_edit *x, EdLspDocumentEdit *change, const char *from, const char *to)
{
    char source[PATH_MAX], target[PATH_MAX]; struct stat st; struct doc *t = NULL;
    language_canon(from, source, sizeof source); language_canon(to, target, sizeof target);
    if (!strcmp(source, target)) return 1;
    if (language_lstat(from, &st) < 0) { language_fail(x->origin, NOTE_ERROR, _("Cannot rename %s: %s."), from, errno == ENOENT ? _("it does not exist") : strerror(errno)); return 0; }
    if (S_ISDIR(st.st_mode) && language_under(target, source)) { language_fail(x->origin, NOTE_ERROR, _("Cannot move the folder %s into itself."), from); return 0; }
    char *backup = NULL;
    if (language_lstat(to, &st) == 0) {
        if (!change->overwrite) {
            if (change->ignore_if_exists) return 1;
            language_fail(x->origin, NOTE_ERROR, _("Cannot rename %s to %s: the target already exists."), from, to); return 0;
        }
        if (S_ISDIR(st.st_mode)) { language_fail(x->origin, NOTE_ERROR, _("Cannot rename onto %s: it is a folder."), to); return 0; }
        if (!language_replaceable(x, to, _("Cannot replace %s: it is open with unsaved changes."), &t)) return 0;
        if (!(backup = language_move_aside(to))) { language_fail(x->origin, NOTE_ERROR, _("Cannot replace %s: %s."), to, strerror(errno)); return 0; }
    } else if (errno != ENOENT) {
        language_fail(x->origin, NOTE_ERROR, _("Cannot rename %s to %s: %s."), from, to, strerror(errno)); return 0;
    } else if (!language_replaceable(x, to, _("Cannot replace %s: it is open with unsaved changes."), &t)) return 0;
    struct language_op *op = language_op_new(x, ED_LSP_EDIT_RENAME, target); op->old_path = g_strdup(source); op->backup = backup;
    op->followed = g_ptr_array_new();
    if (language_rename(from, to) < 0) { language_fail(x->origin, NOTE_ERROR, _("Cannot rename %s to %s: %s."), from, to, strerror(errno)); return 0; }
    op->made = 1;
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i]; char moved[PATH_MAX];
        if (d == t || language_closing(x, d) || (strcmp(d->path, source) && !language_under(d->path, source))) continue;
        if (snprintf(moved, sizeof moved, "%s%s", target, d->path + strlen(source)) >= (int)sizeof moved) { language_fail(x->origin, NOTE_ERROR, _("Cannot rename %s: the new name of %s is too long."), from, d->path); return 0; }
        language_move_doc(d, moved); g_ptr_array_add(op->followed, d);
    }
    /* The target's own unmodified document: closed when a document took its
     * place (as Save As closes the other document), else read again. */
    if (t && op->followed->len) g_ptr_array_add(x->closing, t);
    else if (t) { op->reloaded = t; reload(t); }
    return 1;
}
/* DeleteFile: the folder only when empty unless recursive; each open document
 * of it asked about as closing it asks (Save, Discard, Cancel). */
static int language_delete(struct language_edit *x, EdLspDocumentEdit *change, const char *path)
{
    char canon[PATH_MAX]; struct stat st;
    if (language_lstat(path, &st) < 0) {
        if (errno == ENOENT && change->ignore_if_not_exists) return 1;
        language_fail(x->origin, NOTE_ERROR, _("Cannot delete %s: %s."), path, errno == ENOENT ? _("it does not exist") : strerror(errno)); return 0;
    }
    int dir = S_ISDIR(st.st_mode);
    if (dir && !change->recursive) {
        file_as_user(1); DIR *folder = opendir(path); int e = errno, entries = 0; struct dirent *entry;
        while (folder && (entry = readdir(folder))) entries += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
        if (folder) closedir(folder);
        file_as_user(0);
        if (!folder || entries) { language_fail(x->origin, NOTE_ERROR, _("Cannot delete %s: %s."), path, folder ? _("the folder is not empty") : strerror(e)); return 0; }
    }
    language_canon(path, canon, sizeof canon);
    for (int i = 0; i < ndocs; i++) {
        struct doc *d = docs[i];
        if (!d->path[0] || language_closing(x, d) || (strcmp(d->path, canon) && strcmp(d->path, path) && !(dir && language_under(d->path, canon)))) continue;
        show_doc(active, d);
        if (!query_close(d)) { snprintf(language_failure, sizeof language_failure, _("Deleting %s was cancelled; no changes were applied."), path); return 0; }
        g_ptr_array_add(x->closing, d);
    }
    struct language_op *op = language_op_new(x, ED_LSP_EDIT_DELETE, path); op->backup_dir = dir;
    if (!(op->backup = language_move_aside(path))) { language_fail(x->origin, NOTE_ERROR, _("Cannot delete %s: %s."), path, strerror(errno)); return 0; }
    op->made = 1; return 1;
}
/* Everything done undone, last first: a created file removed (if it is still
 * the one written), a rename and its documents moved back, a moved-aside file
 * put back, documents opened only for the edit closed. Documents asked about
 * for a delete were not closed yet and stay as they are. */
static void language_rollback(struct language_edit *x)
{
    GString *lost = g_string_new(NULL);
    for (guint i = x->ops->len; i > 0; i--) {
        struct language_op *op = g_ptr_array_index(x->ops, i - 1); struct stat st;
        if (op->made && op->kind == ED_LSP_EDIT_CREATE) {
            int same = language_lstat(op->path, &st) == 0 && st.st_ino == op->created.st_ino && st.st_dev == op->created.st_dev;
            file_as_user(1); int removed = same && unlink(op->path) == 0; file_as_user(0);
            if (!removed) g_string_append_printf(lost, _(" %s was not removed."), op->path);
        } else if (op->made && op->kind == ED_LSP_EDIT_RENAME) {
            if (language_rename(op->path, op->old_path) < 0) g_string_append_printf(lost, _(" %s was not renamed back to %s."), op->path, op->old_path);
            for (guint k = 0; k < op->followed->len; k++) {
                struct doc *d = g_ptr_array_index(op->followed, k); char back[PATH_MAX];
                snprintf(back, sizeof back, "%s%s", op->old_path, d->path + strlen(op->path)); language_move_doc(d, back);
            }
        }
        if (op->backup && language_rename(op->backup, op->path) < 0) g_string_append_printf(lost, _(" %s is kept as %s."), op->path, op->backup);
        if (op->reloaded && language_alive(op->reloaded)) reload(op->reloaded);
    }
    for (guint i = 0; i < x->opened->len; i++) {
        struct doc *d = g_ptr_array_index(x->opened, i);
        if (language_alive(d) && !doc_modified(d)) language_close_now(d);
    }
    if (lost->len) {
        size_t n = strlen(language_failure);
        snprintf(language_failure + n, sizeof language_failure - n, _(" Could not undo everything:%s"), lost->str);
        struct doc *note = language_alive(x->origin) ? x->origin : cur_tab() ? cur_tab()->doc : NULL;
        if (note) post(note, NOTE_ERROR, 1, 0, "%s", language_failure);
    }
    g_string_free(lost, TRUE);
}
/* The edit succeeded: documents of deleted or replaced files closed, and what
 * was moved aside removed for good. */
static void language_commit(struct language_edit *x)
{
    for (guint i = 0; i < x->closing->len; i++) {
        struct doc *d = g_ptr_array_index(x->closing, i); if (language_alive(d)) language_close_now(d);
    }
    for (guint i = 0; i < x->ops->len; i++) {
        struct language_op *op = g_ptr_array_index(x->ops, i);
        if (!op->backup) continue;
        file_as_user(1); int r = op->backup_dir ? nftw(op->backup, language_remove_entry, 16, FTW_DEPTH | FTW_PHYS) : unlink(op->backup); int e = errno; file_as_user(0);
        struct doc *note = cur_tab() ? cur_tab()->doc : NULL;
        if (r && note) post(note, NOTE_WARNING, 1, 0, _("Applied, but %s could not be removed: %s"), op->backup, strerror(e));
    }
}
static int language_apply(struct language_server *server, EdLspWorkspaceEdit *edit, uint64_t revision, struct doc *origin)
{
    language_failure[0] = '\0';
    if (!edit) return 1;
    if (revision != ed_lsp_revision(server->client)) { language_fail(origin, NOTE_WARNING, _("Workspace changed; request this edit again.")); return 0; }
    struct language_edit x = { server, origin, g_ptr_array_new_with_free_func(language_stage_free), g_ptr_array_new_with_free_func(language_op_free), g_ptr_array_new(), g_ptr_array_new() };
    GString *summary = g_string_new(NULL); GPtrArray *touched = g_ptr_array_new_with_free_func(g_free);
    GHashTable *text_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean *deferred = g_new0(gboolean, edit->documents->len + 1); int ok = 1;
    for (guint i = 0; edit->annotations && i < edit->annotations->len; i++) {
        EdLspAnnotation *annotation = g_ptr_array_index(edit->annotations, i);
        if (annotation->needs_confirmation) g_string_append_printf(summary, _("Confirmation required: %s\n%s\n"), annotation->label, annotation->description);
        else g_string_append_printf(summary, "%s\n%s\n", annotation->label, annotation->description);
    }
    struct doc *previous = cur_tab() ? cur_tab()->doc : NULL;
    /* Before anything changes: every change well formed and local; text edits
     * of files no earlier operation touches staged now (stale or invalid edits
     * are found before the question), the others when their turn comes. */
    for (guint i = 0; i < edit->documents->len && ok; i++) {
        EdLspDocumentEdit *change = g_ptr_array_index(edit->documents, i);
        char *path = change->kind == ED_LSP_EDIT_INVALID ? NULL : language_path(change->uri), *old = change->kind == ED_LSP_EDIT_RENAME ? language_path(change->old_uri) : NULL;
        if (!path || (change->kind == ED_LSP_EDIT_RENAME && !old)) {
            language_fail(origin, NOTE_ERROR, _("Invalid workspace edit: change %u is malformed or not a local file; no changes were applied."), i + 1); ok = 0;
        } else if (change->kind == ED_LSP_EDIT_TEXT) {
            char canon[PATH_MAX]; language_canon(path, canon, sizeof canon); int later = 0;
            for (guint k = 0; k < touched->len; k++) later |= !strcmp(canon, g_ptr_array_index(touched, k)) || language_under(canon, g_ptr_array_index(touched, k));
            g_hash_table_add(text_paths, g_strdup(canon));
            if (later) deferred[i] = TRUE; else ok = language_stage_text(&x, change, summary);
        } else {
            char canon[PATH_MAX]; language_canon(path, canon, sizeof canon); g_ptr_array_add(touched, g_strdup(canon));
            if (old) { language_canon(old, canon, sizeof canon); g_ptr_array_add(touched, g_strdup(canon)); }
            if (change->kind == ED_LSP_EDIT_CREATE) g_string_append_printf(summary, _("Create %s\n"), path);
            else if (change->kind == ED_LSP_EDIT_RENAME) g_string_append_printf(summary, _("Rename %s to %s\n"), old, path);
            else g_string_append_printf(summary, change->recursive ? _("Delete %s (with its contents)\n") : _("Delete %s\n"), path);
        }
        g_free(path); g_free(old);
    }
    if (previous && language_alive(previous)) show_doc(active, previous);
    if (ok && (g_hash_table_size(text_paths) > 1 || edit->annotations->len)) {
        const char *const choices[] = { _("&Apply"), _("&Cancel") };
        ok = ask(_("Apply Workspace Edit"), summary->str, choices, 2, 0, 1) == 0;
        if (!ok) snprintf(language_failure, sizeof language_failure, "%s", _("The user did not apply the edit."));
    }
    /* In order: resource operations now, deferred text edits staged in turn. */
    for (guint i = 0; i < edit->documents->len && ok; i++) {
        EdLspDocumentEdit *change = g_ptr_array_index(edit->documents, i);
        char *path = language_path(change->uri), *old = change->kind == ED_LSP_EDIT_RENAME ? language_path(change->old_uri) : NULL;
        if (change->kind == ED_LSP_EDIT_TEXT) ok = !deferred[i] || language_stage_text(&x, change, NULL);
        else if (change->kind == ED_LSP_EDIT_CREATE) ok = language_create(&x, change, path);
        else if (change->kind == ED_LSP_EDIT_RENAME) ok = language_rename_file(&x, change, old, path);
        else if (change->kind == ED_LSP_EDIT_DELETE) ok = language_delete(&x, change, path);
        g_free(path); g_free(old);
    }
    if (ok) {
        language_busy++;
        for (guint i = 0; i < x.staged->len; i++) {
            struct language_staged *p = g_ptr_array_index(x.staged, i);
            if (language_closing(&x, p->doc)) continue; /* its file is deleted or replaced */
            doc_safe_point(p->doc);
            struct replacement replacement = {0, doc_len(p->doc), p->after, strlen(p->after)};
            doc_begin(p->doc, p->cursor, p->anchor);
            int result = doc_replace_many(p->doc, &replacement, 1); doc_end(p->doc, MIN(p->cursor, doc_len(p->doc)), MIN(p->anchor, doc_len(p->doc))); doc_safe_point(p->doc);
            if (result < 0) { ok = 0; break; } p->applied = 1;
        }
        if (!ok) {
            for (guint i = x.staged->len; i > 0; i--) { struct language_staged *p = g_ptr_array_index(x.staged, i - 1); if (p->applied) { size_t c, a; doc_undo(p->doc, &c, &a); } }
            language_fail(origin, NOTE_ERROR, _("Workspace edit failed; all changed documents were rolled back."));
        }
        language_busy--;
        for (guint i = 0; i < x.staged->len; i++) { struct language_staged *p = g_ptr_array_index(x.staged, i); if (!language_closing(&x, p->doc)) { language_changed(p->doc); update_name(p->doc); } }
    }
    if (ok) language_commit(&x); else language_rollback(&x);
    if (previous && language_alive(previous) && tab_index(active, previous) >= 0) show_doc(active, previous);
    if (x.ops->len) follow_project();
    if (cur_tab()) show_cursor(active, cur_tab(), 0);
    if (!ok && !language_failure[0]) snprintf(language_failure, sizeof language_failure, "%s", _("The editor rejected the edit."));
    g_string_free(summary, TRUE); g_ptr_array_unref(touched); g_hash_table_unref(text_paths); g_free(deferred);
    g_ptr_array_unref(x.staged); g_ptr_array_unref(x.ops); g_ptr_array_unref(x.closing); g_ptr_array_unref(x.opened); return ok;
}
static void language_command_action(struct language_document *b, const char *command, const char *arguments)
{
    if (!command || !*command) return;
    EdLspRequest r = {.method = ED_LSP_EXECUTE_COMMAND, .command = command, .arguments_json = arguments}; language_send(b, &r, 0);
}
static void language_locations(EdLspResult *result)
{
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; result->items && i < result->items->len; i++) {
        EdLspLocation *l = g_ptr_array_index(result->items, i); char *path = g_filename_from_uri(l->uri, NULL, NULL);
        g_ptr_array_add(labels, g_strdup_printf("%s:%u:%u", path ? path : l->uri, l->selection_range.start.line + 1, l->selection_range.start.character + 1)); g_free(path);
    }
    int selected = labels->len == 1 ? 0 : language_pick(_("Locations"), labels);
    if (selected >= 0) { EdLspLocation *l = g_ptr_array_index(result->items, selected); language_jump(l->uri, l->selection_range); }
    g_ptr_array_unref(labels);
}
static void language_symbol_rows(GPtrArray *symbols, GPtrArray *labels, GPtrArray *flat, unsigned depth)
{
    for (guint i = 0; symbols && i < symbols->len; i++) {
        EdLspSymbol *s = g_ptr_array_index(symbols, i);
        g_ptr_array_add(labels, g_strdup_printf("%*s%s  %s  :%u", (int)MIN(depth, 32U) * 2, "", s->name, s->detail, s->selection_range.start.line + 1)); g_ptr_array_add(flat, s);
        language_symbol_rows(s->children, labels, flat, depth + 1);
    }
}
static void language_completion(struct language_document *b, EdLspEvent *event, struct language_request_ui *request)
{
    EdLspResult *r = event->result; if (!r->items || !r->items->len) { if (!request->automatic) post(b->doc, NOTE_INFO, 0, 3, _("No completions.")); return; }
    struct tab *t = cur_tab(); if (!t || t->doc != b->doc || t->cursor != request->cursor) return;
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < r->items->len; i++) { EdLspCompletion *c = g_ptr_array_index(r->items, i); g_ptr_array_add(labels, g_strdup_printf("%s  %s", c->label, c->detail)); }
    /* TRANSLATORS: Keep key names (Enter/Tab, F1, Esc) as is. */
    language_pick_help = _("Enter/Tab: insert   F1: selected documentation   Esc: cancel");
    int selected = language_pick(r->incomplete ? _("Completion (more results after typing)") : _("Completion"), labels);
    language_pick_help = NULL; g_ptr_array_unref(labels);
    if (selected < 0) return;
    EdLspCompletion *c = g_ptr_array_index(r->items, selected);
    if (language_pick_help_requested) {
        if (ed_lsp_capabilities(b->server->client)->completion_resolve) { EdLspRequest resolve = {.method = ED_LSP_COMPLETION_RESOLVE, .payload_json = c->payload_json}; language_send(b, &resolve, 0); }
        else language_text(c->label, c->documentation && *c->documentation ? c->documentation : _("No documentation."));
        return;
    }
    if (c->snippet) { post(b->doc, NOTE_WARNING, 0, 4, _("Server returned an unrequested snippet; no text inserted.")); return; }
    size_t a = request->cursor, end = request->cursor;
    if (c->has_edit) {
        if (!ed_lsp_position_utf8(b->snapshot, c->edit_range.start, &a) || !ed_lsp_position_utf8(b->snapshot, c->edit_range.end, &end)) return;
    } else word_at(b->doc, request->cursor, &a, &end);
    if (!c->has_edit) end = request->cursor;
    if (c->kind == 2 || c->kind == 3) {
        /* Kate's default inserts parentheses for function/method completions. */
        if (!strchr(c->insert_text, '(') && (end >= doc_len(b->doc) || byte_at(b->doc, end) != '(')) {
            char *with_parentheses = g_strconcat(c->insert_text, "()", NULL); g_free(c->insert_text); c->insert_text = with_parentheses;
        }
    }
    EdLspPosition start_pos, end_pos; if (!ed_lsp_utf8_position(b->snapshot, a, &start_pos) || !ed_lsp_utf8_position(b->snapshot, end, &end_pos)) return;
    EdLspTextEdit main_edit = {.range = {start_pos, end_pos}, .new_text = c->insert_text};
    GPtrArray *edits = g_ptr_array_new(); g_ptr_array_add(edits, &main_edit);
    for (guint i = 0; i < c->additional_edits->len; i++) g_ptr_array_add(edits, g_ptr_array_index(c->additional_edits, i));
    EdLspDocumentEdit document = {.kind = ED_LSP_EDIT_TEXT, .uri = b->uri, .version = event->version, .edits = edits};
    GPtrArray *documents = g_ptr_array_new(), *annotations = g_ptr_array_new(); g_ptr_array_add(documents, &document);
    EdLspWorkspaceEdit workspace = {.documents = documents, .annotations = annotations};
    if (language_apply(b->server, &workspace, event->revision, b->doc)) { size_t cursor = a + strlen(c->insert_text); if (g_str_has_suffix(c->insert_text, "()")) cursor--; t->cursor = t->anchor = MIN(cursor, doc_len(b->doc)); show_cursor(active, t, 0); language_command_action(b, c->command, c->command_arguments_json); }
    g_ptr_array_unref(edits); g_ptr_array_unref(documents); g_ptr_array_unref(annotations);
}
static void language_result(struct language_server *s, EdLspEvent *e, struct language_request_ui *request)
{
    struct language_document *b = language_binding(request->doc);
    if (!b || b->server != s) return;
    if (e->status != ED_LSP_OK) {
        if (!request->automatic && e->status != ED_LSP_CANCELLED && e->status != ED_LSP_STALE) post(b->doc, NOTE_WARNING, 0, 5, _("Language server: %s"), e->message ? e->message : _("request failed"));
        if (b->saving) { b->saving = 0; language_save_resume++; save(b->doc); language_save_resume--; }
        return;
    }
    EdLspResult *r = e->result;
    if (request->automatic && (!cur_tab() || cur_tab()->doc != b->doc || (!request->pointer_hover && cur_tab()->cursor != request->cursor))) return;
    switch (r->method) {
    case ED_LSP_COMPLETION: language_completion(b, e, request); break;
    case ED_LSP_COMPLETION_RESOLVE:
        if (r->items && r->items->len) { EdLspCompletion *c = g_ptr_array_index(r->items, 0); language_text(c->label, c->documentation); } break;
    case ED_LSP_SIGNATURE: {
        GString *text = g_string_new(NULL);
        for (guint i = 0; r->items && i < r->items->len; i++) {
            EdLspSignature *signature = g_ptr_array_index(r->items, i); int parameter = signature->active_parameter >= 0 ? signature->active_parameter : r->active_parameter;
            g_string_append_printf(text, "%s%s\n%s\n", (int)i == r->active_signature ? "> " : "  ", signature->label, signature->documentation);
            if (parameter >= 0 && parameter < (int)signature->parameters->len) { EdLspParameter *p = g_ptr_array_index(signature->parameters, parameter); g_string_append_printf(text, _("Parameter %d: %s\n%s\n"), parameter + 1, p->label, p->documentation); }
        }
        if (request->automatic) { if (text->len) post(b->doc, NOTE_INFO, 0, 5, "%s", text->str); }
        else language_text(_("Signature Help"), text->len ? text->str : _("No signature information."));
        g_string_free(text, TRUE); break;
    }
    case ED_LSP_HOVER:
        if (request->automatic) { if (r->text && *r->text) post(b->doc, NOTE_INFO, 0, 5, "%s", r->text); }
        else language_text(r->markdown ? _("Hover (Markdown)") : _("Hover"), r->text && *r->text ? r->text : _("No hover information."));
        break;
    case ED_LSP_DEFINITION: case ED_LSP_DECLARATION: case ED_LSP_TYPE_DEFINITION: case ED_LSP_IMPLEMENTATION: case ED_LSP_REFERENCES:
        if (!r->items || !r->items->len) post(b->doc, NOTE_INFO, 0, 3, _("No locations found.")); else language_locations(r); break;
    case ED_LSP_DOCUMENT_SYMBOLS: case ED_LSP_WORKSPACE_SYMBOLS: {
        GPtrArray *labels = g_ptr_array_new_with_free_func(g_free), *flat = g_ptr_array_new(); language_symbol_rows(r->items, labels, flat, 0);
        int selected = language_pick(r->method == ED_LSP_DOCUMENT_SYMBOLS ? _("Symbol Outline") : _("Project Symbols"), labels);
        if (selected >= 0) { EdLspSymbol *symbol = g_ptr_array_index(flat, selected); language_jump(symbol->uri && *symbol->uri ? symbol->uri : b->uri, symbol->selection_range); }
        g_ptr_array_unref(labels); g_ptr_array_unref(flat); break;
    }
    case ED_LSP_CODE_ACTION: case ED_LSP_CODE_ACTION_RESOLVE: {
        GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; r->items && i < r->items->len; i++) {
            EdLspCodeAction *a = g_ptr_array_index(r->items, i);
            g_ptr_array_add(labels, *a->disabled_reason ?
                            g_strdup_printf(_("%s%s (disabled: %s)"), a->preferred ? "* " : "", a->title, a->disabled_reason) :
                            g_strdup_printf("%s%s", a->preferred ? "* " : "", a->title));
        }
        int selected = r->method == ED_LSP_CODE_ACTION_RESOLVE && labels->len == 1 ? 0 : language_pick(_("Code Actions / Quick Fix"), labels); g_ptr_array_unref(labels);
        if (selected < 0) break;
        EdLspCodeAction *a = g_ptr_array_index(r->items, selected);
        if (*a->disabled_reason) { post(b->doc, NOTE_INFO, 0, 5, "%s", a->disabled_reason); break; }
        if (!a->edit && (!a->command || !*a->command) && ed_lsp_capabilities(s->client)->action_resolve && r->method != ED_LSP_CODE_ACTION_RESOLVE) {
            EdLspRequest resolve = {.method = ED_LSP_CODE_ACTION_RESOLVE, .payload_json = a->payload_json}; language_send(b, &resolve, 0); break;
        }
        /* A DeleteFile may have closed the document: its binding looked up again. */
        struct doc *doc = b->doc;
        if (language_apply(s, a->edit, e->revision, doc) && (b = language_binding(doc))) language_command_action(b, a->command, a->arguments_json);
        break;
    }
    case ED_LSP_RENAME: case ED_LSP_FORMAT: case ED_LSP_RANGE_FORMAT: case ED_LSP_ON_TYPE_FORMAT: {
        struct doc *doc = b->doc; language_apply(s, r->edit, e->revision, doc);
        if ((b = language_binding(doc)) && b->saving) { b->saving = 0; language_save_resume++; save(b->doc); language_save_resume--; }
        break;
    }
    case ED_LSP_HIGHLIGHT: {
        struct docui *u = ui_of(b->doc); free(u->hl); u->hl = calloc(r->items ? r->items->len * 2 : 0, sizeof *u->hl); u->nhl = 0;
        for (guint i = 0; u->hl && r->items && i < r->items->len; i++) { EdLspHighlight *h = g_ptr_array_index(r->items, i); size_t a, end; if (ed_lsp_position_utf8(b->snapshot, h->range.start, &a) && ed_lsp_position_utf8(b->snapshot, h->range.end, &end)) { u->hl[2 * u->nhl] = a; u->hl[2 * u->nhl++ + 1] = end; } } break;
    }
    case ED_LSP_SWITCH_SOURCE_HEADER: if (r->text && *r->text) language_open_uri(r->text); break;
    case ED_LSP_PREPARE_RENAME: case ED_LSP_EXECUTE_COMMAND: break;
    }
}
static void language_diagnostics_menu(void)
{
    struct tab *t = cur_tab(); if (!t) return; struct language_document *b = language_binding(t->doc);
    if (!b || !b->diagnostics || !b->diagnostics->diagnostics->len) { post(t->doc, NOTE_INFO, 0, 3, _("No diagnostics.")); return; }
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < b->diagnostics->diagnostics->len; i++) { EdLspDiagnostic *d = g_ptr_array_index(b->diagnostics->diagnostics, i); g_ptr_array_add(labels, g_strdup_printf("%u:%u  %s: %s", d->range.start.line + 1, d->range.start.character + 1, d->severity == 1 ? _("Error") : d->severity == 2 ? _("Warning") : _("Info"), d->message)); }
    int selected = language_pick(_("Diagnostics"), labels); g_ptr_array_unref(labels);
    if (selected >= 0) { EdLspDiagnostic *d = g_ptr_array_index(b->diagnostics->diagnostics, selected); language_jump(b->uri, d->range); }
}
static void language_tick(void)
{
    if (language_busy) return;
    language_init();
    if (language_presenting) {
        for (guint i = 0; i < language_servers->len; i++) { struct language_server *s = g_ptr_array_index(language_servers, i); if (s->client) ed_lsp_step(s->client, 0); }
        return;
    }
    for (int i = 0; i < ndocs; i++) {
        struct language_document *b = language_binding(docs[i]);
        if (b) { char *uri = g_filename_to_uri(docs[i]->path, NULL, NULL); if (g_strcmp0(uri, b->uri)) language_closed(docs[i]); g_free(uri); }
        language_attach(docs[i], 0);
    }
    for (guint i = 0; i < language_servers->len; i++) {
        struct language_server *s = g_ptr_array_index(language_servers, i); if (!s->client) continue; ed_lsp_step(s->client, 0);
        EdLspEvent *e;
        while ((e = ed_lsp_next_event(s->client))) {
            if (e->kind == ED_LSP_EVENT_DIAGNOSTICS) {
                struct language_document *b = language_by_uri(s, e->uri);
                if (b) {
                    ed_lsp_event_free(b->diagnostics); b->diagnostics = e;
                    if (language_show_diagnostics && e->diagnostics->len && cur_tab() && cur_tab()->doc == b->doc) {
                        EdLspDiagnostic *d = g_ptr_array_index(e->diagnostics, 0);
                        post(b->doc, d->severity == 1 ? NOTE_ERROR : NOTE_WARNING, 0, 5,
                             P_("%u:%u: %s (%u diagnostic)", "%u:%u: %s (%u diagnostics)", e->diagnostics->len),
                             d->range.start.line + 1, d->range.start.character + 1, d->message, e->diagnostics->len);
                    }
                    continue;
                }
            } else if (e->kind == ED_LSP_EVENT_RESULT) {
                for (guint j = 0; j < language_requests->len; j++) {
                    struct language_request_ui *r = g_ptr_array_index(language_requests, j);
                    if (r->server == s && r->id == e->id) {
                        struct language_request_ui copy = *r; g_ptr_array_remove_index(language_requests, j);
                        language_presenting++; language_result(s, e, &copy); language_presenting--; break;
                    }
                }
            } else if (e->kind == ED_LSP_EVENT_APPLY_EDIT) {
                /* Kate applies it only shortly after its own executeCommand; this
                 * editor applies it (stale edits are refused) and replies with why not. */
                struct doc *origin = cur_tab() ? cur_tab()->doc : NULL;
                language_presenting++; int applied = language_apply(s, e->edit, e->revision, origin); language_presenting--;
                ed_lsp_reply_apply_edit(s->client, e->id, applied, applied ? NULL : language_failure, NULL);
            } else if (e->kind == ED_LSP_EVENT_STATE && e->state == ED_LSP_FAILED && !s->failed_notice) {
                s->failed_notice = 1;
                if (cur_tab()) post(cur_tab()->doc, NOTE_WARNING, 1, 0, _("%s server stopped: %s. Use Restart Current Server. %s"), s->language, e->message ? e->message : _("process exited"), ed_lsp_stderr(s->client));
            }
            ed_lsp_event_free(e);
        }
    }
    struct tab *t = cur_tab(); struct language_document *b = t ? language_binding(t->doc) : NULL;
    if (b && language_trigger_doc == b->doc && language_trigger && *language_trigger && ed_lsp_state(b->server->client) == ED_LSP_READY) {
        const EdLspCapabilities *caps = ed_lsp_capabilities(b->server->client); const char *last = g_utf8_find_prev_char(language_trigger, language_trigger + strlen(language_trigger));
        if (last && g_utf8_strlen(last, -1) == 1) {
            if (language_auto_completion && caps->completion && caps->completion_triggers && strstr(caps->completion_triggers, last)) { EdLspRequest r = {.method = ED_LSP_COMPLETION, .trigger = last}; language_send(b, &r, 1); }
            if (language_auto_signature && caps->signature && ((caps->signature_triggers && strstr(caps->signature_triggers, last)) || (caps->signature_retriggers && strstr(caps->signature_retriggers, last)))) { EdLspRequest r = {.method = ED_LSP_SIGNATURE, .trigger = last, .retrigger = TRUE}; language_send(b, &r, 1); }
            if (language_format_typing && caps->on_type_formatting && caps->format_triggers && strstr(caps->format_triggers, last)) { EdLspRequest r = {.method = ED_LSP_ON_TYPE_FORMAT, .text = last, .insert_spaces = TRUE}; language_send(b, &r, 1); }
        }
    }
    language_trigger_doc = NULL; g_clear_pointer(&language_trigger, g_free);
    if (language_hover_doc && now() >= language_hover_due) {
        struct language_document *hover = language_binding(language_hover_doc);
        if (hover && t && t->doc == hover->doc && ed_lsp_state(hover->server->client) == ED_LSP_READY) {
            EdLspRequest r = {.method = ED_LSP_HOVER, .uri = hover->uri, .offset = language_hover_offset};
            uint64_t id = ed_lsp_request(hover->server->client, &r, NULL);
            if (id) { struct language_request_ui *pending = g_new0(struct language_request_ui, 1); pending->server = hover->server; pending->id = id; pending->doc = hover->doc; pending->cursor = language_hover_offset; pending->automatic = pending->pointer_hover = 1; pending->method = ED_LSP_HOVER; g_ptr_array_add(language_requests, pending); }
        }
        language_hover_doc = NULL;
    }
}
static void language_hover_at(struct doc *d, size_t at)
{
    if (!language_auto_hover || (language_hover_doc == d && language_hover_offset == at)) return;
    language_hover_doc = d; language_hover_offset = at; language_hover_due = now() + 0.6;
}
static void language_saved(struct doc *d)
{
    struct language_document *b = language_binding(d);
    if (b) { char *uri = g_filename_to_uri(d->path, NULL, NULL); if (g_strcmp0(uri, b->uri)) { language_closed(d); b = NULL; } g_free(uri); }
    if (!b) b = language_attach(d, 0);
    if (!b) return;
    language_changed(d); ed_lsp_save(b->server->client, b->uri, NULL);
    /* A separate before-save hook avoids saving a second time recursively. */
}
static int language_before_save(struct doc *d)
{
    if (!language_format_save || language_save_resume) return 0;
    struct language_document *b = language_attach(d, 0);
    if (b && b->saving) return 1;
    if (!b || ed_lsp_state(b->server->client) != ED_LSP_READY || !ed_lsp_capabilities(b->server->client)->formatting) return 0;
    EdLspRequest r = {.method = ED_LSP_FORMAT, .insert_spaces = TRUE};
    if (language_send(b, &r, 0)) { b->saving = 1; return 1; } return 0;
}
static void language_restart(struct doc *d, int all)
{
    struct language_document *b = d ? language_binding(d) : NULL; struct language_server *target = b ? b->server : NULL;
    for (guint i = 0; language_documents && i < language_documents->len;) {
        struct language_document *next = g_ptr_array_index(language_documents, i); if (all || next->server == target) language_closed(next->doc); else i++;
    }
    for (guint i = 0; language_servers && i < language_servers->len;) {
        struct language_server *s = g_ptr_array_index(language_servers, i);
        if (all || !target || s == target) { ed_lsp_free(s->client); g_free(s->key); g_free(s->language); g_free(s->root); g_free(s); g_ptr_array_remove_index(language_servers, i); } else i++;
    }
    if (d) language_attach(d, 1);
}
static void language_configure(void)
{
    struct tab *t = cur_tab(); if (!t) return; language_init();
    const char *type = language_server_type(language_id(t->doc)); char **current = language_command(type);
    GString *command = g_string_new(NULL);
    for (guint i = 0; current && current[i]; i++) { char *quoted = g_shell_quote(current[i]); if (i) g_string_append_c(command, ' '); g_string_append(command, quoted); g_free(quoted); }
    g_strfreev(current); struct field input = {0}; field_set(&input, command->str, command->len); input.all = 1; g_string_free(command, TRUE);
    if (!prompt_line(_("Language server command (argv, no shell):"), &input)) return;
    char **argv = NULL; GError *error = NULL;
    if (!g_shell_parse_argv(input.s, NULL, &argv, &error)) { post(t->doc, NOTE_WARNING, 0, 5, "%s", error ? error->message : _("Invalid command")); g_clear_error(&error); return; }
    JsonObject *root = json_node_get_object(language_settings); JsonNode *servers_node = language_member(language_settings, "servers");
    if (!servers_node || !JSON_NODE_HOLDS_OBJECT(servers_node)) { json_object_set_object_member(root, "servers", json_object_new()); servers_node = language_member(language_settings, "servers"); }
    JsonObject *servers = json_node_get_object(servers_node); JsonNode *old = language_member(servers_node, type);
    JsonObject *config = old && JSON_NODE_HOLDS_OBJECT(old) ? json_object_ref(json_node_get_object(old)) : json_object_new(); JsonArray *array = json_array_new();
    for (guint i = 0; argv[i]; i++) json_array_add_string_element(array, argv[i]);
    json_object_set_array_member(config, "command", array); json_object_set_object_member(servers, type, config);
    g_strfreev(argv); language_write_settings(); language_restart(t->doc, 1);
}
static void language_request_action(EdLspMethod method)
{
    struct tab *t = cur_tab(); if (!t) return; struct language_document *b = language_attach(t->doc, 1); if (!b) return;
    EdLspRequest r = {.method = method, .include_declaration = TRUE, .insert_spaces = TRUE}; struct field input = {0};
    if (method == ED_LSP_RENAME) {
        size_t a, end; word_at(t->doc, t->cursor, &a, &end); char *word = buffer_dup(&t->doc->buf, a, end - a); field_set(&input, word, end - a); free(word); input.all = 1;
        if (!prompt_line(_("New symbol name:"), &input) || !*input.s) return;
        r.text = input.s;
    } else if (method == ED_LSP_WORKSPACE_SYMBOLS) {
        if (!prompt_line(_("Project symbol:"), &input)) return;
        r.text = input.s;
    } else if (method == ED_LSP_CODE_ACTION) {
        r.diagnostics = b->diagnostics ? b->diagnostics->diagnostics : NULL;
    } else if (method == ED_LSP_FORMAT && has_sel(t)) r.method = ED_LSP_RANGE_FORMAT;
    language_send(b, &r, 0);
}
static void language_menu(void)
{
    language_init(); GPtrArray *labels = g_ptr_array_new();
    static const char *const fixed[] = {N_("Complete"), N_("Signature Help"), N_("Hover"), N_("Go to Definition"), N_("Go to Declaration"), N_("Go to Type Definition"), N_("Find Implementations"), N_("Find References (including declaration)"), N_("Symbol Outline"), N_("Project Symbols..."), N_("Highlight References"), N_("Quick Fix / Code Actions"), N_("Rename Symbol..."), N_("Format Document / Selection"), N_("Switch Source / Header"), N_("Show Diagnostics"), N_("Configure Server Command..."), N_("Restart Current Server"), N_("Restart All Servers")};
    for (guint i = 0; i < sizeof fixed / sizeof fixed[0]; i++) g_ptr_array_add(labels, (gpointer)_(fixed[i]));
    char toggles[6][96]; static const char *const names[] = {N_("Automatic Completion"), N_("Automatic Signature Help"), N_("Hover Information"), N_("Format on Save"), N_("Diagnostic Notifications"), N_("Format on Typing")};
    int values[] = {language_auto_completion, language_auto_signature, language_auto_hover, language_format_save, language_show_diagnostics, language_format_typing};
    for (int i = 0; i < 6; i++) { snprintf(toggles[i], sizeof toggles[i], "%s %s", values[i] ? "[x]" : "[ ]", _(names[i])); g_ptr_array_add(labels, toggles[i]); }
    int selected = language_pick(_("Language Server"), labels); g_ptr_array_unref(labels);
    static const EdLspMethod methods[] = {ED_LSP_COMPLETION, ED_LSP_SIGNATURE, ED_LSP_HOVER, ED_LSP_DEFINITION, ED_LSP_DECLARATION, ED_LSP_TYPE_DEFINITION, ED_LSP_IMPLEMENTATION, ED_LSP_REFERENCES, ED_LSP_DOCUMENT_SYMBOLS, ED_LSP_WORKSPACE_SYMBOLS, ED_LSP_HIGHLIGHT, ED_LSP_CODE_ACTION, ED_LSP_RENAME, ED_LSP_FORMAT, ED_LSP_SWITCH_SOURCE_HEADER};
    if (selected >= 0 && selected < (int)(sizeof methods / sizeof methods[0])) language_request_action(methods[selected]);
    else if (selected == 15) language_diagnostics_menu();
    else if (selected == 16) language_configure();
    else if (selected == 17 || selected == 18) language_restart(cur_tab() ? cur_tab()->doc : NULL, selected == 18);
    else if (selected >= 19 && selected <= 24) {
        int *options[] = {&language_auto_completion, &language_auto_signature, &language_auto_hover, &language_format_save, &language_show_diagnostics, &language_format_typing}; *options[selected - 19] = !*options[selected - 19]; language_write_settings();
    }
}
static int language_key(int key, int mods)
{
    if (!cur_tab()) return 0;
    if (key == ' ' && (mods & (KEYS_CTRL | KEYS_ALT)) == (KEYS_CTRL | KEYS_ALT)) { language_request_action(ED_LSP_COMPLETION); return 1; }
    if (key == K_F1 + 11 && !(mods & (KEYS_CTRL | KEYS_ALT))) { language_request_action(mods & KEYS_SHIFT ? ED_LSP_DECLARATION : ED_LSP_DEFINITION); return 1; }
    if ((key == 'k' || key == 'K') && (mods & (KEYS_CTRL | KEYS_ALT)) == (KEYS_CTRL | KEYS_ALT)) { language_menu(); return 1; }
    if (key == K_F1 + 1 && (mods & KEYS_CTRL)) { language_request_action(ED_LSP_RENAME); return 1; }
    if (key == '\n' && (mods & KEYS_ALT)) { language_request_action(ED_LSP_CODE_ACTION); return 1; }
    return 0;
}
static void language_done(void)
{
    if (!language_initialized) return;
    for (guint i = 0; i < language_servers->len; i++) { struct language_server *s = g_ptr_array_index(language_servers, i); if (s->client) ed_lsp_shutdown(s->client); }
    int64_t until = g_get_monotonic_time() + 2000000;
    while (g_get_monotonic_time() < until) {
        int running = 0;
        for (guint i = 0; i < language_servers->len; i++) { struct language_server *s = g_ptr_array_index(language_servers, i); if (!s->client) continue; ed_lsp_step(s->client, 0); EdLspState state = ed_lsp_state(s->client); running |= state != ED_LSP_EXITED && state != ED_LSP_FAILED; }
        if (!running) break;
        poll(NULL, 0, 5);
    }
    while (language_documents->len) { struct language_document *b = g_ptr_array_index(language_documents, 0); language_closed(b->doc); }
    for (guint i = 0; i < language_servers->len; i++) { struct language_server *s = g_ptr_array_index(language_servers, i); ed_lsp_free(s->client); g_free(s->key); g_free(s->language); g_free(s->root); g_free(s); }
    g_ptr_array_unref(language_servers); g_ptr_array_unref(language_documents); g_ptr_array_unref(language_requests);
    language_servers = language_documents = language_requests = NULL; json_node_free(language_settings); language_settings = NULL; language_initialized = 0; g_clear_pointer(&language_trigger, g_free);
}
