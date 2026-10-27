/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "base.h"
#include "edit-lsp.h"
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STDERR_LIMIT 16384
#define HEADER_LIMIT 8192
#define PENDING_LIMIT 256
#define EVENT_LIMIT 1024
#define INTERNAL_INITIALIZE (-1)
#define INTERNAL_SHUTDOWN (-2)
typedef struct { char *uri, *language, *text; int64_t version; gboolean sent; } Document;
typedef struct {
    uint64_t id, revision; int method; char *uri; int64_t version, deadline;
} Pending;
typedef struct { uint64_t token; JsonNode *id; int64_t deadline; } ApplyRequest;
struct EdLsp {
    GPid pid;
    int in_fd, out_fd, err_fd;
    EdLspState state;
    EdLspCapabilities caps;
    GHashTable *documents, *pending, *applies;
    GQueue events;
    GByteArray *input, *output;
    size_t written, content_length, max_message;
    GString *stderr_text;
    uint64_t next_id, revision;
    unsigned timeout_ms, shutdown_ms;
    int64_t stop_deadline;
    int kill_stage;
    gboolean have_header, save_supported, save_text, open_close, overflow;
    JsonNode *settings;
};

static int64_t now_ms(void) { return g_get_monotonic_time() / 1000; }
static GQuark domain(void) { return g_quark_from_static_string("edit-lsp"); }
static gboolean fail(GError **e, const char *message) {
    g_set_error_literal(e, domain(), 1, message); return FALSE;
}
static JsonNode *obj(void) { JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, json_object_new()); return n; }
static JsonNode *arr(void) { JsonNode *n = json_node_new(JSON_NODE_ARRAY); json_node_take_array(n, json_array_new()); return n; }
static JsonNode *str(const char *s) { return json_node_init_string(json_node_new(JSON_NODE_VALUE), s ? s : ""); }
static JsonNode *num(int64_t i) { return json_node_init_int(json_node_new(JSON_NODE_VALUE), i); }
static JsonNode *boolean(gboolean b) { return json_node_init_boolean(json_node_new(JSON_NODE_VALUE), b); }
static JsonNode *null_node(void) { return json_node_new(JSON_NODE_NULL); }
static void put(JsonNode *o, const char *key, JsonNode *v) { json_object_set_member(json_node_get_object(o), key, v); }
static void add(JsonNode *a, JsonNode *v) { json_array_add_element(json_node_get_array(a), v); }
static JsonNode *get(JsonNode *o, const char *key) {
    return o && JSON_NODE_HOLDS_OBJECT(o) ? json_object_get_member(json_node_get_object(o), key) : NULL;
}
static const char *string(JsonNode *n) { return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : ""; }
static gboolean integer(JsonNode *n) { return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64; }
static int64_t number(JsonNode *n, int64_t fallback) { return integer(n) ? json_node_get_int(n) : fallback; }
static gboolean truth(JsonNode *n) { return n && ((JSON_NODE_HOLDS_OBJECT(n)) || (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n))); }
static guint length(JsonNode *a) { return a && JSON_NODE_HOLDS_ARRAY(a) ? json_array_get_length(json_node_get_array(a)) : 0; }
static JsonNode *at(JsonNode *a, guint i) { return json_array_get_element(json_node_get_array(a), i); }
static char *serialize(JsonNode *n) { return json_to_string(n, FALSE); }
static JsonNode *parse(const char *text, gssize len, GError **error) {
    JsonParser *p = json_parser_new(); JsonNode *n = NULL;
    if (json_parser_load_from_data(p, text, len, error)) n = json_node_copy(json_parser_get_root(p));
    g_object_unref(p); return n;
}
static char *dup_member(JsonNode *n, const char *key) { return g_strdup(string(get(n, key))); }
static EdLspPosition position(JsonNode *n) {
    int64_t line = number(get(n, "line"), -1), character = number(get(n, "character"), -1);
    EdLspPosition p = {UINT32_MAX, UINT32_MAX};
    if (line >= 0 && line <= INT32_MAX && character >= 0 && character <= INT32_MAX) {
        p.line = (uint32_t)line; p.character = (uint32_t)character;
    }
    return p;
}
static EdLspRange range(JsonNode *n) {
    EdLspRange r = {position(get(n, "start")), position(get(n, "end"))};
    if (r.end.line < r.start.line || (r.end.line == r.start.line && r.end.character < r.start.character))
        r.start = r.end = (EdLspPosition){UINT32_MAX, UINT32_MAX};
    return r;
}
static JsonNode *pos_node(EdLspPosition p) { JsonNode *n = obj(); put(n, "line", num(p.line)); put(n, "character", num(p.character)); return n; }
static JsonNode *range_node(EdLspRange r) { JsonNode *n = obj(); put(n, "start", pos_node(r.start)); put(n, "end", pos_node(r.end)); return n; }

/* LSP treats CRLF as one line ending and excludes both bytes from the line. */
gboolean ed_lsp_utf8_position(const char *text, size_t offset, EdLspPosition *out) {
    if (!text || !out || !g_utf8_validate(text, -1, NULL) || offset > strlen(text)) return FALSE;
    EdLspPosition p = {0, 0}; size_t i = 0;
    while (i < offset) {
        if (text[i] == '\r' || text[i] == '\n') {
            size_t step = text[i] == '\r' && text[i + 1] == '\n' ? 2 : 1;
            if (i + step > offset || p.line == UINT32_MAX) return FALSE;
            i += step; p.line++; p.character = 0;
        } else {
            gunichar c = g_utf8_get_char(text + i); size_t step = (size_t)(g_utf8_next_char(text + i) - (text + i));
            if (i + step > offset || p.character > UINT32_MAX - (c > 0xffff ? 2U : 1U)) return FALSE;
            i += step; p.character += c > 0xffff ? 2 : 1;
        }
    }
    *out = p; return TRUE;
}
gboolean ed_lsp_position_utf8(const char *text, EdLspPosition target, size_t *out) {
    if (!text || !out || !g_utf8_validate(text, -1, NULL)) return FALSE;
    EdLspPosition p = {0, 0}; size_t i = 0;
    for (;;) {
        if (p.line == target.line && p.character == target.character) { *out = i; return TRUE; }
        if (!text[i] || p.line > target.line || (p.line == target.line && p.character > target.character)) return FALSE;
        if (text[i] == '\r' || text[i] == '\n') {
            if (p.line == target.line) return FALSE;
            i += text[i] == '\r' && text[i + 1] == '\n' ? 2 : 1; p.line++; p.character = 0;
        } else {
            p.character += g_utf8_get_char(text + i) > 0xffff ? 2 : 1;
            i = (size_t)(g_utf8_next_char(text + i) - text);
        }
    }
}
static void text_edit_free(gpointer p) { EdLspTextEdit *x = p; g_free(x->new_text); g_free(x->annotation_id); g_free(x); }
static void document_edit_free(gpointer p) { EdLspDocumentEdit *x = p; g_free(x->uri); g_free(x->old_uri); g_free(x->annotation_id); g_ptr_array_unref(x->edits); g_free(x); }
static void annotation_free(gpointer p) { EdLspAnnotation *x = p; g_free(x->id); g_free(x->label); g_free(x->description); g_free(x); }
static EdLspWorkspaceEdit *workspace_new(void) {
    EdLspWorkspaceEdit *w = g_new0(EdLspWorkspaceEdit, 1);
    w->documents = g_ptr_array_new_with_free_func(document_edit_free);
    w->annotations = g_ptr_array_new_with_free_func(annotation_free); return w;
}
static void workspace_free(EdLspWorkspaceEdit *w) { if (w) { g_ptr_array_unref(w->documents); g_ptr_array_unref(w->annotations); g_free(w); } }
static EdLspDocumentEdit *document_edit_new(const char *uri) {
    EdLspDocumentEdit *d = g_new0(EdLspDocumentEdit, 1); d->uri = g_strdup(uri); d->version = -1;
    d->edits = g_ptr_array_new_with_free_func(text_edit_free); return d;
}
static EdLspTextEdit *text_edit(JsonNode *n) {
    EdLspTextEdit *e = g_new0(EdLspTextEdit, 1); e->range = range(get(n, "range"));
    JsonNode *text = get(n, "newText");
    e->new_text = text && json_node_get_value_type(text) == G_TYPE_STRING ? g_strdup(string(text)) : NULL;
    e->annotation_id = dup_member(n, "annotationId"); return e;
}
static void decode_edits(GPtrArray *edits, JsonNode *a) {
    for (guint i = 0; i < length(a); i++) g_ptr_array_add(edits, text_edit(at(a, i)));
}
/* A resource operation's option: absent is false; anything but a boolean
 * makes the operation malformed. */
static gboolean option(JsonNode *options, const char *key, gboolean *out) {
    JsonNode *v = get(options, key); *out = FALSE;
    if (!v) return TRUE;
    if (!JSON_NODE_HOLDS_VALUE(v) || json_node_get_value_type(v) != G_TYPE_BOOLEAN) return FALSE;
    *out = json_node_get_boolean(v); return TRUE;
}
/* CreateFile, RenameFile, DeleteFile (LSP 3.17 "File Resource changes"):
 * create/rename take overwrite and ignoreIfExists, delete takes recursive and
 * ignoreIfNotExists. A missing URI or a non-object/non-boolean option is
 * malformed and rejected as a whole. */
static void resource_operation(EdLspDocumentEdit *d, JsonNode *v) {
    JsonNode *opts = get(v, "options"); gboolean ok = !opts || JSON_NODE_HOLDS_OBJECT(opts);
    g_free(d->uri); d->uri = dup_member(v, d->kind == ED_LSP_EDIT_RENAME ? "newUri" : "uri");
    if (d->kind == ED_LSP_EDIT_RENAME) { d->old_uri = dup_member(v, "oldUri"); ok = ok && *d->old_uri; }
    if (d->kind == ED_LSP_EDIT_DELETE) ok = ok && option(opts, "recursive", &d->recursive) && option(opts, "ignoreIfNotExists", &d->ignore_if_not_exists);
    else ok = ok && option(opts, "overwrite", &d->overwrite) && option(opts, "ignoreIfExists", &d->ignore_if_exists);
    if (!ok || !*d->uri) d->kind = ED_LSP_EDIT_INVALID;
}
static EdLspWorkspaceEdit *workspace(JsonNode *n) {
    if (!n || !JSON_NODE_HOLDS_OBJECT(n)) return NULL;
    EdLspWorkspaceEdit *w = workspace_new(); JsonNode *changes = get(n, "changes");
    /* The specification gives documentChanges precedence when both occur. */
    if (!get(n, "documentChanges") && changes && JSON_NODE_HOLDS_OBJECT(changes)) {
        GList *keys = json_object_get_members(json_node_get_object(changes));
        for (GList *k = keys; k; k = k->next) {
            EdLspDocumentEdit *d = document_edit_new(k->data);
            if (!JSON_NODE_HOLDS_ARRAY(get(changes, k->data))) d->kind = ED_LSP_EDIT_INVALID;
            decode_edits(d->edits, get(changes, k->data)); g_ptr_array_add(w->documents, d);
        }
        g_list_free(keys);
    }
    JsonNode *a = get(n, "documentChanges");
    for (guint i = 0; i < length(a); i++) {
        JsonNode *v = at(a, i), *td = get(v, "textDocument");
        EdLspDocumentEdit *d = document_edit_new(td ? string(get(td, "uri")) : string(get(v, "uri")));
        d->version = number(get(td, "version"), -1); d->annotation_id = dup_member(v, "annotationId");
        const char *kind = string(get(v, "kind"));
        if (!strcmp(kind, "create")) d->kind = ED_LSP_EDIT_CREATE;
        else if (!strcmp(kind, "rename")) d->kind = ED_LSP_EDIT_RENAME;
        else if (!strcmp(kind, "delete")) d->kind = ED_LSP_EDIT_DELETE;
        else if (*kind || !td || !JSON_NODE_HOLDS_ARRAY(get(v, "edits"))) d->kind = ED_LSP_EDIT_INVALID;
        if (d->kind == ED_LSP_EDIT_CREATE || d->kind == ED_LSP_EDIT_RENAME || d->kind == ED_LSP_EDIT_DELETE) {
            d->version = -1; resource_operation(d, v); g_ptr_array_add(w->documents, d); continue;
        }
        JsonNode *version_node = get(td, "version");
        if (version_node && !JSON_NODE_HOLDS_NULL(version_node) && (!integer(version_node) || d->version < 0 || d->version > INT32_MAX)) { d->kind = ED_LSP_EDIT_INVALID; d->version = -2; }
        decode_edits(d->edits, get(v, "edits")); g_ptr_array_add(w->documents, d);
    }
    JsonNode *annotations = get(n, "changeAnnotations");
    if (annotations && JSON_NODE_HOLDS_OBJECT(annotations)) {
        GList *keys = json_object_get_members(json_node_get_object(annotations));
        for (GList *k = keys; k; k = k->next) {
            JsonNode *v = get(annotations, k->data); EdLspAnnotation *x = g_new0(EdLspAnnotation, 1);
            x->id = g_strdup(k->data); x->label = dup_member(v, "label"); x->description = dup_member(v, "description");
            x->needs_confirmation = truth(get(v, "needsConfirmation")); g_ptr_array_add(w->annotations, x);
        }
        g_list_free(keys);
    }
    return w;
}
typedef struct { size_t start, end; guint index; const char *text; } OffsetEdit;
static gint compare_edit(gconstpointer aa, gconstpointer bb) {
    const OffsetEdit *a = aa, *b = bb;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return a->index < b->index ? -1 : a->index != b->index;
}
char *ed_lsp_apply_text_edits(const char *text, int64_t version, const EdLspDocumentEdit *edit, GError **error) {
    if (!text || !g_utf8_validate(text, -1, NULL) || !edit || !edit->edits || edit->kind != ED_LSP_EDIT_TEXT || edit->version < -1 || (edit->version >= 0 && edit->version != version)) { fail(error, _("Invalid or stale text edit")); return NULL; }
    GArray *a = g_array_new(FALSE, FALSE, sizeof(OffsetEdit));
    for (guint i = 0; i < edit->edits->len; i++) {
        EdLspTextEdit *e = g_ptr_array_index(edit->edits, i); OffsetEdit o = {.index = i, .text = e->new_text};
        if (!e->new_text || !g_utf8_validate(e->new_text, -1, NULL) || !ed_lsp_position_utf8(text, e->range.start, &o.start) || !ed_lsp_position_utf8(text, e->range.end, &o.end) || o.end < o.start) { g_array_unref(a); fail(error, _("Invalid text edit range or UTF-8")); return NULL; }
        g_array_append_val(a, o);
    }
    g_array_sort(a, compare_edit); GString *out = g_string_new(NULL); size_t cursor = 0;
    for (guint i = 0; i < a->len; i++) {
        OffsetEdit *e = &g_array_index(a, OffsetEdit, i);
        if (e->start < cursor) { g_string_free(out, TRUE); g_array_unref(a); fail(error, _("Overlapping text edits")); return NULL; }
        g_string_append_len(out, text + cursor, e->start - cursor); g_string_append(out, e->text); cursor = e->end;
    }
    g_string_append(out, text + cursor); g_array_unref(a); return g_string_free(out, FALSE);
}

static void location_free(gpointer p) { EdLspLocation *x = p; g_free(x->uri); g_free(x); }
static EdLspLocation *location(JsonNode *n) {
    EdLspLocation *l = g_new0(EdLspLocation, 1); gboolean link = get(n, "targetUri") != NULL;
    l->uri = dup_member(n, link ? "targetUri" : "uri");
    l->range = range(get(n, link ? "targetRange" : "range"));
    l->selection_range = link ? range(get(n, "targetSelectionRange")) : l->range;
    l->has_origin = get(n, "originSelectionRange") != NULL; l->origin_range = range(get(n, "originSelectionRange")); return l;
}
static void symbol_free(gpointer p) {
    EdLspSymbol *s = p; g_free(s->name); g_free(s->detail); g_free(s->container_name); g_free(s->uri); g_ptr_array_unref(s->children); g_free(s);
}
static EdLspSymbol *symbol(JsonNode *n, const char *uri, unsigned depth) {
    EdLspSymbol *s = g_new0(EdLspSymbol, 1); s->name = dup_member(n, "name"); s->detail = dup_member(n, "detail");
    s->container_name = dup_member(n, "containerName"); s->kind = (int)number(get(n, "kind"), 0); s->deprecated = truth(get(n, "deprecated"));
    JsonNode *loc = get(n, "location"); s->uri = g_strdup(loc ? string(get(loc, "uri")) : uri);
    s->range = range(get(loc ? loc : n, "range")); s->selection_range = get(n, "selectionRange") ? range(get(n, "selectionRange")) : s->range;
    s->children = g_ptr_array_new_with_free_func(symbol_free); JsonNode *a = get(n, "children");
    if (depth < 128) for (guint i = 0; i < length(a); i++) g_ptr_array_add(s->children, symbol(at(a, i), uri, depth + 1));
    return s;
}
static void diagnostic_free(gpointer p) {
    EdLspDiagnostic *d = p; g_free(d->message); g_free(d->source); g_free(d->code); g_free(d->code_uri); g_free(d->payload_json);
    g_ptr_array_unref(d->related); g_ptr_array_unref(d->related_messages); g_free(d);
}
static EdLspDiagnostic *diagnostic(JsonNode *n) {
    EdLspDiagnostic *d = g_new0(EdLspDiagnostic, 1); d->range = range(get(n, "range"));
    d->message = dup_member(n, "message"); d->source = dup_member(n, "source"); d->severity = (int)number(get(n, "severity"), 0);
    JsonNode *code = get(n, "code"); d->code = integer(code) ? g_strdup_printf("%" G_GINT64_FORMAT, number(code, 0)) : g_strdup(string(code));
    d->code_uri = dup_member(get(n, "codeDescription"), "href"); d->payload_json = serialize(n);
    JsonNode *tags = get(n, "tags");
    for (guint i = 0; i < length(tags); i++) { d->unnecessary |= number(at(tags, i), 0) == 1; d->deprecated |= number(at(tags, i), 0) == 2; }
    d->related = g_ptr_array_new_with_free_func(location_free); d->related_messages = g_ptr_array_new_with_free_func(g_free);
    JsonNode *a = get(n, "relatedInformation");
    for (guint i = 0; i < length(a); i++) { g_ptr_array_add(d->related, location(get(at(a, i), "location"))); g_ptr_array_add(d->related_messages, dup_member(at(a, i), "message")); }
    return d;
}
static GPtrArray *diagnostics(JsonNode *a) {
    GPtrArray *out = g_ptr_array_new_with_free_func(diagnostic_free);
    for (guint i = 0; i < length(a); i++) g_ptr_array_add(out, diagnostic(at(a, i)));
    return out;
}
static char *documentation(JsonNode *n, gboolean *markdown) {
    if (n && JSON_NODE_HOLDS_OBJECT(n)) { *markdown = !strcmp(string(get(n, "kind")), "markdown") || *string(get(n, "language")); return dup_member(n, "value"); }
    return g_strdup(string(n));
}
static void command_fields(JsonNode *n, char **command, char **arguments) {
    *command = dup_member(n, "command"); JsonNode *a = get(n, "arguments"); *arguments = a ? serialize(a) : g_strdup("[]");
}
static void completion_free(gpointer p) {
    EdLspCompletion *c = p; g_free(c->label); g_free(c->detail); g_free(c->documentation); g_free(c->sort_text); g_free(c->filter_text); g_free(c->insert_text);
    g_free(c->command); g_free(c->command_arguments_json); g_free(c->payload_json); g_ptr_array_unref(c->additional_edits); g_free(c);
}
static EdLspCompletion *completion(JsonNode *n, JsonNode *defaults) {
    EdLspCompletion *c = g_new0(EdLspCompletion, 1); c->label = dup_member(n, "label"); c->detail = dup_member(n, "detail");
    c->documentation = documentation(get(n, "documentation"), &c->documentation_markdown);
    c->sort_text = dup_member(n, "sortText"); c->filter_text = dup_member(n, "filterText");
    JsonNode *te = get(n, "textEdit"), *dr = get(defaults, "editRange");
    c->insert_text = g_strdup(te ? string(get(te, "newText")) : get(n, "textEditText") ? string(get(n, "textEditText")) : get(n, "insertText") ? string(get(n, "insertText")) : c->label);
    c->kind = (int)number(get(n, "kind"), 0); c->preselect = truth(get(n, "preselect")); c->deprecated = truth(get(n, "deprecated"));
    c->snippet = number(get(n, "insertTextFormat"), number(get(defaults, "insertTextFormat"), 1)) == 2;
    c->has_edit = te || dr;
    if (te) { c->has_insert_range = get(te, "insert") != NULL; c->edit_range = range(get(te, c->has_insert_range ? "replace" : "range")); c->insert_range = range(get(te, "insert")); }
    else if (dr) { c->has_insert_range = get(dr, "insert") != NULL; c->edit_range = range(c->has_insert_range ? get(dr, "replace") : dr); c->insert_range = range(get(dr, "insert")); }
    c->additional_edits = g_ptr_array_new_with_free_func(text_edit_free); decode_edits(c->additional_edits, get(n, "additionalTextEdits"));
    command_fields(get(n, "command"), &c->command, &c->command_arguments_json);
    JsonNode *payload = json_node_copy(n); if (!get(payload, "data") && get(defaults, "data")) put(payload, "data", json_node_copy(get(defaults, "data")));
    c->payload_json = serialize(payload); json_node_free(payload); return c;
}
static void parameter_free(gpointer p) { EdLspParameter *x = p; g_free(x->label); g_free(x->documentation); g_free(x); }
static void signature_free(gpointer p) { EdLspSignature *s = p; g_free(s->label); g_free(s->documentation); g_ptr_array_unref(s->parameters); g_free(s); }
static EdLspSignature *signature(JsonNode *n) {
    EdLspSignature *s = g_new0(EdLspSignature, 1); s->label = dup_member(n, "label"); s->documentation = documentation(get(n, "documentation"), &s->documentation_markdown);
    s->active_parameter = (int)number(get(n, "activeParameter"), -1); s->parameters = g_ptr_array_new_with_free_func(parameter_free);
    JsonNode *a = get(n, "parameters");
    for (guint i = 0; i < length(a); i++) {
        JsonNode *v = at(a, i), *label = get(v, "label"); EdLspParameter *p = g_new0(EdLspParameter, 1);
        p->label = g_strdup(string(label)); p->has_label_offsets = length(label) == 2;
        if (p->has_label_offsets) { p->label_start = (uint32_t)number(at(label, 0), 0); p->label_end = (uint32_t)number(at(label, 1), 0); }
        p->documentation = documentation(get(v, "documentation"), &p->documentation_markdown); g_ptr_array_add(s->parameters, p);
    }
    return s;
}
static void action_free(gpointer p) {
    EdLspCodeAction *a = p; g_free(a->title); g_free(a->kind); g_free(a->disabled_reason); workspace_free(a->edit);
    g_free(a->command); g_free(a->arguments_json); g_free(a->payload_json); g_ptr_array_unref(a->diagnostics); g_free(a);
}
static EdLspCodeAction *action(JsonNode *n) {
    EdLspCodeAction *a = g_new0(EdLspCodeAction, 1); a->title = dup_member(n, "title"); a->kind = dup_member(n, "kind");
    a->disabled_reason = dup_member(get(n, "disabled"), "reason"); a->preferred = truth(get(n, "isPreferred"));
    a->edit = workspace(get(n, "edit")); JsonNode *c = get(n, "command"); command_fields(c && JSON_NODE_HOLDS_OBJECT(c) ? c : n, &a->command, &a->arguments_json);
    a->payload_json = serialize(n); a->diagnostics = diagnostics(get(n, "diagnostics")); return a;
}
static void result_free(EdLspResult *r) { if (r) { if (r->items) g_ptr_array_unref(r->items); g_free(r->text); workspace_free(r->edit); g_free(r); } }
static EdLspResult *decode_result(EdLspMethod method, JsonNode *n, const char *uri) {
    EdLspResult *r = g_new0(EdLspResult, 1); r->method = method; r->active_signature = r->active_parameter = -1;
    switch (method) {
    case ED_LSP_COMPLETION: case ED_LSP_COMPLETION_RESOLVE: {
        r->items = g_ptr_array_new_with_free_func(completion_free); JsonNode *a = get(n, "items"), *defaults = get(n, "itemDefaults"); r->incomplete = truth(get(n, "isIncomplete"));
        if (method == ED_LSP_COMPLETION_RESOLVE) { if (n && JSON_NODE_HOLDS_OBJECT(n)) g_ptr_array_add(r->items, completion(n, NULL)); }
        else { if (!a) a = n; for (guint i = 0; i < length(a); i++) g_ptr_array_add(r->items, completion(at(a, i), defaults)); } break;
    }
    case ED_LSP_SIGNATURE: {
        r->items = g_ptr_array_new_with_free_func(signature_free); JsonNode *a = get(n, "signatures");
        for (guint i = 0; i < length(a); i++) g_ptr_array_add(r->items, signature(at(a, i)));
        r->active_signature = (int)number(get(n, "activeSignature"), 0); r->active_parameter = (int)number(get(n, "activeParameter"), 0); break;
    }
    case ED_LSP_HOVER: {
        JsonNode *contents = get(n, "contents"); GString *text = g_string_new(NULL);
        if (contents && JSON_NODE_HOLDS_ARRAY(contents)) {
            for (guint i = 0; i < length(contents); i++) { gboolean md = FALSE; char *s = documentation(at(contents, i), &md); if (i) g_string_append(text, "\n\n"); g_string_append(text, s); g_free(s); r->markdown |= md; }
        } else { char *s = documentation(contents, &r->markdown); g_string_append(text, s); g_free(s); }
        r->text = g_string_free(text, FALSE); r->has_range = get(n, "range") != NULL; r->range = range(get(n, "range")); break;
    }
    case ED_LSP_DEFINITION: case ED_LSP_DECLARATION: case ED_LSP_TYPE_DEFINITION: case ED_LSP_IMPLEMENTATION: case ED_LSP_REFERENCES:
        r->items = g_ptr_array_new_with_free_func(location_free);
        if (n && JSON_NODE_HOLDS_OBJECT(n)) g_ptr_array_add(r->items, location(n));
        else for (guint i = 0; i < length(n); i++) g_ptr_array_add(r->items, location(at(n, i)));
        break;
    case ED_LSP_DOCUMENT_SYMBOLS: case ED_LSP_WORKSPACE_SYMBOLS:
        r->items = g_ptr_array_new_with_free_func(symbol_free);
        for (guint i = 0; i < length(n); i++) g_ptr_array_add(r->items, symbol(at(n, i), uri, 0));
        break;
    case ED_LSP_CODE_ACTION: case ED_LSP_CODE_ACTION_RESOLVE:
        r->items = g_ptr_array_new_with_free_func(action_free);
        if (method == ED_LSP_CODE_ACTION_RESOLVE) { if (n && JSON_NODE_HOLDS_OBJECT(n)) g_ptr_array_add(r->items, action(n)); }
        else for (guint i = 0; i < length(n); i++) g_ptr_array_add(r->items, action(at(n, i)));
        break;
    case ED_LSP_RENAME: r->edit = workspace(n); break;
    case ED_LSP_FORMAT: case ED_LSP_RANGE_FORMAT: case ED_LSP_ON_TYPE_FORMAT: {
        r->edit = workspace_new(); EdLspDocumentEdit *d = document_edit_new(uri); decode_edits(d->edits, n); g_ptr_array_add(r->edit->documents, d); break;
    }
    case ED_LSP_PREPARE_RENAME:
        r->has_range = n && JSON_NODE_HOLDS_OBJECT(n) && !truth(get(n, "defaultBehavior"));
        r->range = range(get(n, "range") ? get(n, "range") : n); r->text = dup_member(n, "placeholder"); break;
    case ED_LSP_HIGHLIGHT:
        r->items = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < length(n); i++) { EdLspHighlight *h = g_new0(EdLspHighlight, 1); h->range = range(get(at(n, i), "range")); h->kind = (int)number(get(at(n, i), "kind"), 1); g_ptr_array_add(r->items, h); } break;
    case ED_LSP_SWITCH_SOURCE_HEADER: r->text = g_strdup(string(n)); break;
    case ED_LSP_EXECUTE_COMMAND: break;
    }
    return r;
}
void ed_lsp_event_free(EdLspEvent *e) { if (e) { g_free(e->uri); g_free(e->message); result_free(e->result); if (e->diagnostics) g_ptr_array_unref(e->diagnostics); workspace_free(e->edit); g_free(e); } }
static void document_free(gpointer p) { Document *d = p; g_free(d->uri); g_free(d->language); g_free(d->text); g_free(d); }
static void pending_free(gpointer p) { Pending *r = p; g_free(r->uri); g_free(r); }
static void apply_free(gpointer p) { ApplyRequest *a = p; json_node_free(a->id); g_free(a); }
static void queue_event(EdLsp *c, EdLspEvent *e) {
    /* A caller that never drains events must not exhaust memory indefinitely. */
    if (c->events.length >= EVENT_LIMIT) { ed_lsp_event_free(e); c->overflow = TRUE; return; }
    g_queue_push_tail(&c->events, e);
}
static void state_event(EdLsp *c, EdLspState state, const char *message) {
    c->state = state; EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_STATE; e->state = state; e->message = g_strdup(message); queue_event(c, e);
}
static void result_event(EdLsp *c, Pending *p, EdLspStatus status, const char *message, int code, JsonNode *result) {
    if (p->method < 0) return;
    EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_RESULT; e->id = p->id; e->status = status; e->uri = g_strdup(p->uri);
    e->version = p->version; e->revision = p->revision; e->message = g_strdup(message); e->error_code = code;
    e->result = status == ED_LSP_OK ? decode_result((EdLspMethod)p->method, result, p->uri) : g_new0(EdLspResult, 1);
    e->result->method = (EdLspMethod)p->method;
    if (e->result->edit && p->method != ED_LSP_RENAME && e->result->edit->documents->len) ((EdLspDocumentEdit *)g_ptr_array_index(e->result->edit->documents, 0))->version = p->version;
    queue_event(c, e);
}
static void close_fd(int *fd) { if (*fd >= 0) close(*fd); *fd = -1; }
static void abort_client(EdLsp *c, const char *message) {
    if (c->state == ED_LSP_FAILED || c->state == ED_LSP_EXITED) return;
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, c->pending);
    while (g_hash_table_iter_next(&it, NULL, &value)) { result_event(c, value, ED_LSP_TRANSPORT_ERROR, message, 0, NULL); g_hash_table_iter_remove(&it); }
    if (c->pid > 0) { kill(-c->pid, SIGKILL); kill(c->pid, SIGKILL); }
    g_hash_table_remove_all(c->applies);
    close_fd(&c->in_fd); close_fd(&c->out_fd); close_fd(&c->err_fd); state_event(c, ED_LSP_FAILED, message);
}
static gboolean send_node(EdLsp *c, JsonNode *n) {
    if (c->state == ED_LSP_FAILED || c->state == ED_LSP_EXITED) { json_node_free(n); return FALSE; }
    char *text = serialize(n); size_t len = strlen(text); json_node_free(n);
    if (len > c->max_message || c->output->len - c->written + len > c->max_message * 2) { g_free(text); abort_client(c, _("LSP output queue limit exceeded")); return FALSE; }
    if (c->written) { g_byte_array_remove_range(c->output, 0, (guint)c->written); c->written = 0; }
    char header[80]; int h = g_snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
    g_byte_array_append(c->output, (guint8 *)header, (guint)h); g_byte_array_append(c->output, (guint8 *)text, (guint)len); g_free(text); return TRUE;
}
static JsonNode *rpc(const char *method, JsonNode *params) { JsonNode *n = obj(); put(n, "jsonrpc", str("2.0")); put(n, "method", str(method)); if (params) put(n, "params", params); return n; }
static gboolean notify(EdLsp *c, const char *method, JsonNode *params) { return send_node(c, rpc(method, params)); }
static gboolean reply(EdLsp *c, JsonNode *id, JsonNode *result, int code, const char *message) {
    JsonNode *n = obj(); put(n, "jsonrpc", str("2.0")); put(n, "id", json_node_copy(id));
    if (code) { JsonNode *e = obj(); put(e, "code", num(code)); put(e, "message", str(message)); put(n, "error", e); if (result) json_node_free(result); }
    else put(n, "result", result ? result : null_node());
    return send_node(c, n);
}
static uint64_t request_node(EdLsp *c, int method, const char *name, JsonNode *params, Document *d) {
    Pending *p = g_new0(Pending, 1); p->id = ++c->next_id; p->method = method; p->deadline = now_ms() + c->timeout_ms;
    p->uri = d ? g_strdup(d->uri) : NULL; p->version = d ? d->version : -1; p->revision = c->revision;
    JsonNode *n = rpc(name, params); put(n, "id", num((int64_t)p->id)); uint64_t id = p->id;
    if (!send_node(c, n)) { pending_free(p); return 0; } g_hash_table_insert(c->pending, &p->id, p); return id;
}
static JsonNode *text_document(Document *d, gboolean versioned) { JsonNode *n = obj(); put(n, "uri", str(d->uri)); if (versioned) put(n, "version", num(d->version)); return n; }
static gboolean send_open(EdLsp *c, Document *d) {
    if (!c->open_close) { d->sent = TRUE; return TRUE; }
    JsonNode *p = obj(), *td = text_document(d, TRUE); put(td, "languageId", str(d->language)); put(td, "text", str(d->text)); put(p, "textDocument", td);
    d->sent = notify(c, "textDocument/didOpen", p); return d->sent;
}
static char *triggers(JsonNode *a) { GString *s = g_string_new(NULL); for (guint i = 0; i < length(a); i++) g_string_append(s, string(at(a, i))); return g_string_free(s, FALSE); }
static void initialize_result(EdLsp *c, JsonNode *n) {
    JsonNode *caps = get(n, "capabilities"), *encoding = get(caps, "positionEncoding");
    if (!caps || (*string(encoding) && strcmp(string(encoding), "utf-16"))) { abort_client(c, _("Server did not negotiate UTF-16 positions")); return; }
#define CAP(field, key) c->caps.field = truth(get(caps, key))
    CAP(completion, "completionProvider"); CAP(signature, "signatureHelpProvider"); CAP(hover, "hoverProvider"); CAP(definition, "definitionProvider"); CAP(declaration, "declarationProvider"); CAP(type_definition, "typeDefinitionProvider");
    CAP(implementation, "implementationProvider"); CAP(references, "referencesProvider"); CAP(document_symbols, "documentSymbolProvider"); CAP(workspace_symbols, "workspaceSymbolProvider"); CAP(highlight, "documentHighlightProvider");
    CAP(code_action, "codeActionProvider"); CAP(rename, "renameProvider"); CAP(formatting, "documentFormattingProvider"); CAP(range_formatting, "documentRangeFormattingProvider"); CAP(on_type_formatting, "documentOnTypeFormattingProvider");
#undef CAP
    c->caps.prepare_rename = truth(get(get(caps, "renameProvider"), "prepareProvider"));
    c->caps.completion_resolve = truth(get(get(caps, "completionProvider"), "resolveProvider")); c->caps.action_resolve = truth(get(get(caps, "codeActionProvider"), "resolveProvider"));
    c->caps.completion_triggers = triggers(get(get(caps, "completionProvider"), "triggerCharacters"));
    c->caps.signature_triggers = triggers(get(get(caps, "signatureHelpProvider"), "triggerCharacters")); c->caps.signature_retriggers = triggers(get(get(caps, "signatureHelpProvider"), "retriggerCharacters"));
    JsonNode *fmt = get(caps, "documentOnTypeFormattingProvider"); char *more = triggers(get(fmt, "moreTriggerCharacter")); c->caps.format_triggers = g_strconcat(string(get(fmt, "firstTriggerCharacter")), more, NULL); g_free(more);
    c->caps.server_name = dup_member(get(n, "serverInfo"), "name"); c->caps.server_version = dup_member(get(n, "serverInfo"), "version");
    JsonNode *sync = get(caps, "textDocumentSync"); c->caps.sync_kind = (int)number(sync, number(get(sync, "change"), 0));
    if (c->caps.sync_kind < 0 || c->caps.sync_kind > 2) { abort_client(c, _("Invalid text document synchronization capability")); return; }
    c->open_close = integer(sync) ? c->caps.sync_kind != 0 : truth(get(sync, "openClose"));
    JsonNode *save = get(sync, "save"); c->save_supported = truth(save); c->save_text = truth(get(save, "includeText"));
    if (!notify(c, "initialized", obj())) return;
    if (c->settings) { JsonNode *p = obj(); put(p, "settings", json_node_copy(c->settings)); if (!notify(c, "workspace/didChangeConfiguration", p)) return; }
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, c->documents);
    while (g_hash_table_iter_next(&it, NULL, &value)) if (!send_open(c, value)) return;
    state_event(c, ED_LSP_READY, _("Language server initialized"));
}
static gboolean workspace_sensitive(int method) {
    return method == ED_LSP_RENAME || method == ED_LSP_CODE_ACTION || method == ED_LSP_CODE_ACTION_RESOLVE || method == ED_LSP_EXECUTE_COMMAND;
}
static void server_request(EdLsp *c, JsonNode *message, const char *method) {
    JsonNode *id = get(message, "id"), *params = get(message, "params");
    if (!strcmp(method, "workspace/configuration")) {
        JsonNode *a = arr(), *items = get(params, "items");
        for (guint i = 0; i < length(items); i++) {
            JsonNode *v = c->settings; const char *section = string(get(at(items, i), "section")); char **parts = g_strsplit(section, ".", -1);
            if (*section) for (guint k = 0; parts[k] && v; k++) v = get(v, parts[k]);
            add(a, v ? json_node_copy(v) : null_node()); g_strfreev(parts);
        }
        reply(c, id, a, 0, NULL);
    } else if (!strcmp(method, "workspace/workspaceFolders")) {
        /* Folder support is not advertised; rootUri is the configured project. */
        reply(c, id, null_node(), 0, NULL);
    } else if (!strcmp(method, "workspace/applyEdit")) {
        if (g_hash_table_size(c->applies) >= PENDING_LIMIT || c->state != ED_LSP_READY) { reply(c, id, NULL, -32603, "Editor is not accepting workspace edits"); return; }
        ApplyRequest *a = g_new0(ApplyRequest, 1); a->token = ++c->next_id; a->id = json_node_copy(id); a->deadline = now_ms() + c->timeout_ms;
        g_hash_table_insert(c->applies, &a->token, a);
        EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_APPLY_EDIT; e->id = a->token; e->revision = c->revision;
        e->edit = workspace(get(params, "edit")); e->message = dup_member(params, "label"); queue_event(c, e);
    } else if (!strcmp(method, "window/workDoneProgress/create")) reply(c, id, null_node(), 0, NULL);
    else if (!strcmp(method, "window/showMessageRequest")) reply(c, id, null_node(), 0, NULL);
    else reply(c, id, NULL, -32601, "Client method is not supported");
}
static void dispatch(EdLsp *c, JsonNode *message) {
    if (!JSON_NODE_HOLDS_OBJECT(message) || strcmp(string(get(message, "jsonrpc")), "2.0")) { abort_client(c, _("Invalid JSON-RPC message")); return; }
    const char *method = string(get(message, "method")); JsonNode *id = get(message, "id"), *params = get(message, "params");
    if (*method && id) { server_request(c, message, method); return; }
    if (*method) {
        if (!strcmp(method, "textDocument/publishDiagnostics")) {
            const char *uri = string(get(params, "uri")); Document *d = g_hash_table_lookup(c->documents, uri);
            int64_t version = number(get(params, "version"), -1);
            if (!d || (version >= 0 && version != d->version)) return;
            for (GList *l = c->events.head; l;) { GList *next = l->next; EdLspEvent *old = l->data; if (old->kind == ED_LSP_EVENT_DIAGNOSTICS && !g_strcmp0(old->uri, uri)) { g_queue_delete_link(&c->events, l); ed_lsp_event_free(old); } l = next; }
            EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_DIAGNOSTICS; e->uri = g_strdup(uri); e->version = version; e->revision = c->revision;
            e->diagnostics = diagnostics(get(params, "diagnostics")); queue_event(c, e);
        } else if (!strcmp(method, "window/logMessage") || !strcmp(method, "window/showMessage")) {
            EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_MESSAGE; e->message = dup_member(params, "message"); e->error_code = (int)number(get(params, "type"), 0); queue_event(c, e);
        }
        return;
    }
    if (!integer(id)) return;
    uint64_t key = (uint64_t)number(id, 0); Pending *p = g_hash_table_lookup(c->pending, &key); if (!p) return;
    /* Steal before callbacks that can abort the client and clear the table. */
    g_hash_table_steal(c->pending, &key); JsonNode *err = get(message, "error"), *result = get(message, "result");
    if ((!err && !result) || (err && result)) { pending_free(p); abort_client(c, _("Invalid JSON-RPC response")); return; }
    if (p->method == INTERNAL_INITIALIZE) {
        if (err || !result) abort_client(c, _("Language server initialization failed")); else initialize_result(c, result);
    } else if (p->method == INTERNAL_SHUTDOWN) {
        notify(c, "exit", NULL); c->stop_deadline = now_ms() + c->shutdown_ms;
    } else {
        Document *d = p->uri ? g_hash_table_lookup(c->documents, p->uri) : NULL;
        gboolean stale = (p->uri && (!d || d->version != p->version)) || (workspace_sensitive(p->method) && p->revision != c->revision);
        result_event(c, p, stale ? ED_LSP_STALE : err ? ED_LSP_SERVER_ERROR : ED_LSP_OK,
                     err ? string(get(err, "message")) : stale ? _("Document changed while request was pending") : NULL,
                     (int)number(get(err, "code"), 0), result);
    }
    pending_free(p);
}
static gboolean valid_wire_value(JsonNode *n, unsigned depth) {
    if (!n || depth > 128) return FALSE;
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) {
        return g_utf8_validate(json_node_get_string(n), -1, NULL);
    }
    if (JSON_NODE_HOLDS_ARRAY(n)) {
        for (guint i = 0; i < length(n); i++) if (!valid_wire_value(at(n, i), depth + 1)) return FALSE;
    } else if (JSON_NODE_HOLDS_OBJECT(n)) {
        GList *keys = json_object_get_members(json_node_get_object(n)); gboolean ok = TRUE;
        for (GList *k = keys; k && ok; k = k->next) ok = valid_wire_value(get(n, k->data), depth + 1);
        g_list_free(keys); return ok;
    }
    return TRUE;
}
/* Reject U+0000 before the mature JSON parser normalizes it to a C string.
 * This is not a JSON tokenizer: an even number of preceding backslashes means
 * the apparent escape is a literal backslash ("\\\\u0000") and must stay legal. */
static gboolean contains_nul_escape(const guint8 *data, size_t size) {
    size_t slashes = 0;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\\') { slashes++; continue; }
        if (data[i] == 'u' && (slashes & 1) && i + 4 < size && !memcmp(data + i + 1, "0000", 4)) return TRUE;
        slashes = 0;
    }
    return FALSE;
}
static void consume_input(EdLsp *c) {
    while (c->input->len && c->state != ED_LSP_FAILED) {
        if (!c->have_header) {
            guint boundary = 0; gboolean found = FALSE;
            for (guint i = 0; i + 3 < c->input->len; i++) if (!memcmp(c->input->data + i, "\r\n\r\n", 4)) { boundary = i; found = TRUE; break; }
            if (!found) { if (c->input->len > HEADER_LIMIT) abort_client(c, _("LSP header exceeds limit")); return; }
            if (boundary > HEADER_LIMIT || memchr(c->input->data, 0, boundary)) { abort_client(c, _("Invalid LSP header")); return; }
            char *header = g_strndup((char *)c->input->data, boundary); char **lines = g_strsplit(header, "\r\n", -1); gboolean have_length = FALSE; size_t size = 0;
            for (guint i = 0; lines[i]; i++) {
                char *colon = strchr(lines[i], ':'); if (!colon) { size = c->max_message + 1; break; }
                *colon++ = 0;
                if (!g_ascii_strcasecmp(lines[i], "Content-Length")) {
                    char *value = g_strstrip(colon), *end = NULL; errno = 0; guint64 n = g_ascii_strtoull(value, &end, 10);
                    if (have_length || !g_ascii_isdigit(*value) || errno || *end || !n || n > c->max_message) { size = c->max_message + 1; break; }
                    have_length = TRUE; size = (size_t)n;
                }
            }
            g_strfreev(lines); g_free(header);
            if (!have_length || size > c->max_message) { abort_client(c, _("Invalid or oversized Content-Length")); return; }
            c->content_length = size; c->have_header = TRUE; g_byte_array_remove_range(c->input, 0, boundary + 4);
        }
        if (c->input->len < c->content_length) return;
        GError *error = NULL;
        if (memchr(c->input->data, 0, c->content_length) || contains_nul_escape(c->input->data, c->content_length)) { abort_client(c, _("NUL in JSON-RPC body")); return; }
        JsonNode *n = parse((char *)c->input->data, (gssize)c->content_length, &error);
        g_byte_array_remove_range(c->input, 0, (guint)c->content_length); c->have_header = FALSE;
        if (!n) { g_clear_error(&error); abort_client(c, _("Malformed JSON-RPC body")); return; }
        if (!valid_wire_value(n, 0)) { json_node_free(n); abort_client(c, _("Invalid or excessively nested JSON-RPC value")); return; }
        dispatch(c, n); json_node_free(n);
    }
}
/* Block SIGPIPE only during a pipe write, preserving the caller's signal policy. */
static ssize_t pipe_write(int fd, const void *data, size_t len) {
    sigset_t block, previous, pending; sigemptyset(&block); sigaddset(&block, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &block, &previous) < 0) return -1;
    sigpending(&pending); gboolean was_pending = sigismember(&pending, SIGPIPE); ssize_t n = write(fd, data, len); int saved = errno;
    if (n < 0 && saved == EPIPE && !was_pending) { struct timespec zero = {0, 0}; while (sigtimedwait(&block, NULL, &zero) < 0 && errno == EINTR) {} }
    sigprocmask(SIG_SETMASK, &previous, NULL); errno = saved; return n;
}
static void child_setup(gpointer unused) { (void)unused; setpgid(0, 0); }
static void nonblock(int fd) { int flags = fcntl(fd, F_GETFL); if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC); }
EdLsp *ed_lsp_new(const EdLspConfig *config, GError **error) {
    if (!config || !config->argv || !config->argv[0] || !*config->argv[0]) { fail(error, _("Language server command is required")); return NULL; }
    EdLsp *c = g_new0(EdLsp, 1); c->in_fd = c->out_fd = c->err_fd = -1; c->state = ED_LSP_STARTING;
    c->documents = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, document_free);
    c->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, pending_free);
    c->applies = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, apply_free);
    c->input = g_byte_array_new(); c->output = g_byte_array_new(); c->stderr_text = g_string_new(NULL);
    c->timeout_ms = config->request_timeout_ms ? MIN(config->request_timeout_ms, 600000U) : 10000;
    c->shutdown_ms = config->shutdown_timeout_ms ? MIN(config->shutdown_timeout_ms, 10000U) : 1500;
    c->max_message = config->max_message_bytes ? CLAMP(config->max_message_bytes, 1024, 64 * 1024 * 1024) : 16 * 1024 * 1024;
    JsonNode *options = NULL;
    if (config->initialization_options_json && !(options = parse(config->initialization_options_json, -1, error))) { ed_lsp_free(c); return NULL; }
    if (config->settings_json && !(c->settings = parse(config->settings_json, -1, error))) { if (options) json_node_free(options); ed_lsp_free(c); return NULL; }
    if (!g_spawn_async_with_pipes(config->cwd, (char **)config->argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                   child_setup, NULL, &c->pid, &c->in_fd, &c->out_fd, &c->err_fd, error)) {
        if (options) json_node_free(options);
        ed_lsp_free(c); return NULL;
    }
    nonblock(c->in_fd); nonblock(c->out_fd); nonblock(c->err_fd);
    JsonNode *p = obj(); put(p, "processId", num(getpid())); put(p, "rootUri", config->root_uri ? str(config->root_uri) : null_node());
    JsonNode *info = obj(); put(info, "name", str("LOCachy edit")); put(info, "version", str("1")); put(p, "clientInfo", info);
    /* Advertise only data we decode and policies the editor can enforce. */
    const char *cap_json = "{\"general\":{\"positionEncodings\":[\"utf-16\"]},\"workspace\":{\"applyEdit\":true,\"configuration\":true,\"workspaceEdit\":{\"documentChanges\":true,\"resourceOperations\":[\"create\",\"rename\",\"delete\"],\"failureHandling\":\"undo\",\"changeAnnotationSupport\":{}},\"symbol\":{}},\"textDocument\":{\"synchronization\":{\"didSave\":true},\"completion\":{\"completionItem\":{\"snippetSupport\":false,\"documentationFormat\":[\"markdown\",\"plaintext\"],\"deprecatedSupport\":true,\"preselectSupport\":true,\"insertReplaceSupport\":true,\"resolveSupport\":{\"properties\":[\"documentation\",\"detail\",\"additionalTextEdits\"]}}},\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},\"signatureHelp\":{\"signatureInformation\":{\"documentationFormat\":[\"markdown\",\"plaintext\"],\"parameterInformation\":{\"labelOffsetSupport\":true},\"activeParameterSupport\":true},\"contextSupport\":true},\"definition\":{\"linkSupport\":true},\"declaration\":{\"linkSupport\":true},\"typeDefinition\":{\"linkSupport\":true},\"implementation\":{\"linkSupport\":true},\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},\"publishDiagnostics\":{\"relatedInformation\":true,\"versionSupport\":true,\"tagSupport\":{\"valueSet\":[1,2]},\"codeDescriptionSupport\":true,\"dataSupport\":true},\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[\"\",\"quickfix\",\"refactor\",\"refactor.extract\",\"refactor.inline\",\"refactor.rewrite\",\"source\",\"source.organizeImports\"]}},\"isPreferredSupport\":true,\"disabledSupport\":true,\"dataSupport\":true,\"resolveSupport\":{\"properties\":[\"edit\"]}},\"rename\":{\"prepareSupport\":true}}}";
    put(p, "capabilities", parse(cap_json, -1, NULL)); if (options) put(p, "initializationOptions", options);
    request_node(c, INTERNAL_INITIALIZE, "initialize", p, NULL); return c;
}
EdLspState ed_lsp_state(const EdLsp *c) { return c->state; }
const EdLspCapabilities *ed_lsp_capabilities(const EdLsp *c) { return &c->caps; }
const char *ed_lsp_stderr(const EdLsp *c) { return c->stderr_text->str; }
uint64_t ed_lsp_revision(const EdLsp *c) { return c->revision; }
EdLspEvent *ed_lsp_next_event(EdLsp *c) { return g_queue_pop_head(&c->events); }
static void send_cancel(EdLsp *c, uint64_t id) { JsonNode *p = obj(); put(p, "id", num((int64_t)id)); notify(c, "$/cancelRequest", p); }
void ed_lsp_cancel(EdLsp *c, uint64_t id) {
    Pending *p = g_hash_table_lookup(c->pending, &id); if (!p || p->method < 0) return;
    g_hash_table_steal(c->pending, &id); send_cancel(c, id); result_event(c, p, ED_LSP_CANCELLED, _("Request cancelled"), 0, NULL); pending_free(p);
}
static void invalidate(EdLsp *c, const char *uri) {
    c->revision++; GArray *ids = g_array_new(FALSE, FALSE, sizeof(uint64_t)); GHashTableIter it; gpointer value;
    g_hash_table_iter_init(&it, c->pending);
    while (g_hash_table_iter_next(&it, NULL, &value)) { Pending *p = value; if (p->method >= 0 && (!g_strcmp0(p->uri, uri) || workspace_sensitive(p->method))) g_array_append_val(ids, p->id); }
    for (guint i = 0; i < ids->len; i++) {
        uint64_t id = g_array_index(ids, uint64_t, i); Pending *p = g_hash_table_lookup(c->pending, &id); if (!p) continue;
        g_hash_table_steal(c->pending, &id); send_cancel(c, id); result_event(c, p, ED_LSP_STALE, _("Document changed while request was pending"), 0, NULL); pending_free(p);
    }
    g_array_unref(ids);
    /* Drop already queued responses too: the UI may not have consumed them yet. */
    for (GList *l = c->events.head; l;) {
        GList *next = l->next; EdLspEvent *e = l->data;
        if (e->kind == ED_LSP_EVENT_DIAGNOSTICS && !g_strcmp0(e->uri, uri)) {
            g_queue_delete_link(&c->events, l); ed_lsp_event_free(e);
        } else if (e->kind == ED_LSP_EVENT_RESULT && e->status == ED_LSP_OK && (!g_strcmp0(e->uri, uri) || workspace_sensitive(e->result->method))) {
            EdLspMethod m = e->result->method; result_free(e->result); e->result = g_new0(EdLspResult, 1); e->result->method = m; e->status = ED_LSP_STALE;
        }
        l = next;
    }
    EdLspEvent *e = g_new0(EdLspEvent, 1); e->kind = ED_LSP_EVENT_DIAGNOSTICS; e->uri = g_strdup(uri); e->version = -1; e->revision = c->revision; e->diagnostics = diagnostics(NULL); queue_event(c, e);
}
static gboolean mutable(EdLsp *c, GError **error) { return (c->state == ED_LSP_STARTING || c->state == ED_LSP_READY) ? TRUE : fail(error, _("Language server is not running")); }
static gboolean local_uri(const char *uri) {
    if (!uri || !g_utf8_validate(uri, -1, NULL)) return FALSE;
    char *host = NULL; char *path = g_filename_from_uri(uri, &host, NULL);
    gboolean valid = path && g_path_is_absolute(path) && (!host || !*host || !strcmp(host, "localhost"));
    g_free(path); g_free(host); return valid;
}
gboolean ed_lsp_open(EdLsp *c, const char *uri, const char *language, const char *text, int64_t version, GError **error) {
    if (!mutable(c, error)) return FALSE;
    if (!local_uri(uri) || !language || !g_utf8_validate(language, -1, NULL) || !text || !g_utf8_validate(text, -1, NULL) || version < 0 || version > INT32_MAX || strlen(text) > c->max_message / 2 || g_hash_table_contains(c->documents, uri)) return fail(error, _("Invalid or already open document"));
    Document *d = g_new0(Document, 1); d->uri = g_strdup(uri); d->language = g_strdup(language); d->text = g_strdup(text); d->version = version;
    g_hash_table_insert(c->documents, d->uri, d); invalidate(c, uri); return c->state == ED_LSP_STARTING || send_open(c, d);
}
const char *ed_lsp_document(EdLsp *c, const char *uri, int64_t *version) { Document *d = g_hash_table_lookup(c->documents, uri); if (!d) return NULL; if (version) *version = d->version; return d->text; }
gboolean ed_lsp_change(EdLsp *c, const char *uri, size_t start, size_t end, const char *replacement, int64_t version, GError **error) {
    if (!mutable(c, error)) return FALSE;
    Document *d = g_hash_table_lookup(c->documents, uri); EdLspRange r;
    if (!d || !replacement || !g_utf8_validate(replacement, -1, NULL) || version <= d->version || version > INT32_MAX || end < start || !ed_lsp_utf8_position(d->text, start, &r.start) || !ed_lsp_utf8_position(d->text, end, &r.end)) return fail(error, _("Invalid document change or version"));
    size_t size = strlen(d->text) - (end - start) + strlen(replacement); if (size > c->max_message / 2) return fail(error, _("Document exceeds message limit"));
    GString *s = g_string_new_len(d->text, start); g_string_append(s, replacement); g_string_append(s, d->text + end); g_free(d->text); d->text = g_string_free(s, FALSE); d->version = version; invalidate(c, uri);
    if (c->state != ED_LSP_READY || !d->sent || !c->caps.sync_kind) return TRUE;
    JsonNode *p = obj(), *changes = arr(), *change = obj(); put(p, "textDocument", text_document(d, TRUE));
    if (c->caps.sync_kind == 2) { put(change, "range", range_node(r)); put(change, "text", str(replacement)); } else put(change, "text", str(d->text));
    add(changes, change); put(p, "contentChanges", changes); return notify(c, "textDocument/didChange", p);
}
gboolean ed_lsp_save(EdLsp *c, const char *uri, GError **error) {
    Document *d = g_hash_table_lookup(c->documents, uri); if (!mutable(c, error)) return FALSE; if (!d) return fail(error, _("Document is not open"));
    if (c->state != ED_LSP_READY || !d->sent || !c->save_supported) return TRUE;
    JsonNode *p = obj(); put(p, "textDocument", text_document(d, FALSE)); if (c->save_text) put(p, "text", str(d->text)); return notify(c, "textDocument/didSave", p);
}
gboolean ed_lsp_close(EdLsp *c, const char *uri, GError **error) {
    Document *d = g_hash_table_lookup(c->documents, uri); if (!mutable(c, error)) return FALSE; if (!d) return fail(error, _("Document is not open"));
    gboolean ok = TRUE; if (c->state == ED_LSP_READY && d->sent && c->open_close) { JsonNode *p = obj(); put(p, "textDocument", text_document(d, FALSE)); ok = notify(c, "textDocument/didClose", p); }
    invalidate(c, uri); g_hash_table_remove(c->documents, uri); return ok;
}
static const char *method_names[] = {"textDocument/completion", "completionItem/resolve", "textDocument/signatureHelp", "textDocument/hover", "textDocument/definition", "textDocument/declaration", "textDocument/typeDefinition", "textDocument/implementation", "textDocument/references", "textDocument/documentSymbol", "workspace/symbol", "textDocument/documentHighlight", "textDocument/codeAction", "codeAction/resolve", "textDocument/rename", "textDocument/prepareRename", "textDocument/formatting", "textDocument/rangeFormatting", "textDocument/onTypeFormatting", "workspace/executeCommand", "textDocument/switchSourceHeader"};
uint64_t ed_lsp_request(EdLsp *c, const EdLspRequest *r, GError **error) {
    if (!r || (unsigned)r->method >= G_N_ELEMENTS(method_names) || c->state != ED_LSP_READY || g_hash_table_size(c->pending) >= PENDING_LIMIT) { fail(error, _("Server is not ready or request limit reached")); return 0; }
    gboolean resolve = r->method == ED_LSP_COMPLETION_RESOLVE || r->method == ED_LSP_CODE_ACTION_RESOLVE;
    gboolean no_doc = r->method == ED_LSP_WORKSPACE_SYMBOLS || r->method == ED_LSP_EXECUTE_COMMAND || resolve;
    Document *d = r->uri ? g_hash_table_lookup(c->documents, r->uri) : NULL; EdLspRange range_value = {0};
    if ((!no_doc && !d) || (r->uri && !d)) { fail(error, _("Document is not open")); return 0; }
    gboolean has_pos = !no_doc && r->method != ED_LSP_DOCUMENT_SYMBOLS && r->method != ED_LSP_FORMAT && r->method != ED_LSP_SWITCH_SOURCE_HEADER;
    if (has_pos && (!ed_lsp_utf8_position(d->text, r->offset, &range_value.start))) { fail(error, _("Invalid UTF-8 request offset")); return 0; }
    gboolean has_range = r->method == ED_LSP_CODE_ACTION || r->method == ED_LSP_RANGE_FORMAT;
    if (has_range && (r->end_offset < r->offset || !ed_lsp_utf8_position(d->text, r->end_offset, &range_value.end))) { fail(error, _("Invalid UTF-8 request range")); return 0; }
    JsonNode *p = obj();
    if (resolve) {
        json_node_free(p); p = r->payload_json ? parse(r->payload_json, -1, error) : NULL;
        if (!p || !JSON_NODE_HOLDS_OBJECT(p)) { if (p) json_node_free(p); if (!error || !*error) fail(error, _("Resolve requires an object payload")); return 0; }
    } else if (r->method == ED_LSP_WORKSPACE_SYMBOLS) put(p, "query", str(r->text));
    else if (r->method == ED_LSP_EXECUTE_COMMAND) {
        if (!r->command || !*r->command) { json_node_free(p); fail(error, _("Command is required")); return 0; }
        put(p, "command", str(r->command)); JsonNode *a = r->arguments_json ? parse(r->arguments_json, -1, error) : arr();
        if (!a || !JSON_NODE_HOLDS_ARRAY(a)) { if (a) json_node_free(a); json_node_free(p); if (!error || !*error) fail(error, _("Command arguments must be an array")); return 0; } put(p, "arguments", a);
    } else {
        put(p, "textDocument", text_document(d, FALSE));
        if (has_range) put(p, "range", range_node(range_value)); else if (has_pos) put(p, "position", pos_node(range_value.start));
        if (r->method == ED_LSP_COMPLETION || r->method == ED_LSP_SIGNATURE) {
            JsonNode *ctx = obj(); put(ctx, "triggerKind", num(r->trigger && *r->trigger ? 2 : r->retrigger ? 3 : 1));
            if (r->trigger && *r->trigger) put(ctx, "triggerCharacter", str(r->trigger));
            if (r->method == ED_LSP_SIGNATURE) put(ctx, "isRetrigger", boolean(r->retrigger));
            put(p, "context", ctx);
        }
        if (r->method == ED_LSP_REFERENCES) { JsonNode *ctx = obj(); put(ctx, "includeDeclaration", boolean(r->include_declaration)); put(p, "context", ctx); }
        if (r->method == ED_LSP_RENAME) { if (!r->text || !*r->text) { json_node_free(p); fail(error, _("Rename requires a new name")); return 0; } put(p, "newName", str(r->text)); }
        if (r->method == ED_LSP_CODE_ACTION) {
            JsonNode *ctx = obj(), *a = arr();
            for (guint i = 0; r->diagnostics && i < r->diagnostics->len; i++) {
                EdLspDiagnostic *diag = g_ptr_array_index(r->diagnostics, i); JsonNode *v = diag->payload_json ? parse(diag->payload_json, -1, NULL) : NULL; if (v) add(a, v);
            }
            put(ctx, "diagnostics", a); put(ctx, "triggerKind", num(1)); if (r->only_kind) { JsonNode *only = arr(); add(only, str(r->only_kind)); put(ctx, "only", only); } put(p, "context", ctx);
        }
        if (r->method == ED_LSP_FORMAT || r->method == ED_LSP_RANGE_FORMAT || r->method == ED_LSP_ON_TYPE_FORMAT) {
            JsonNode *options = obj(); put(options, "tabSize", num(r->tab_size ? r->tab_size : 4)); put(options, "insertSpaces", boolean(r->insert_spaces)); put(p, "options", options);
            if (r->method == ED_LSP_ON_TYPE_FORMAT) put(p, "ch", str(r->text));
        }
    }
    return request_node(c, r->method, method_names[r->method], p, d);
}
gboolean ed_lsp_reply_apply_edit(EdLsp *c, uint64_t token, gboolean applied, const char *reason, GError **error) {
    ApplyRequest *a = g_hash_table_lookup(c->applies, &token); if (!a) return fail(error, _("Workspace edit request has expired"));
    JsonNode *result = obj(); put(result, "applied", boolean(applied)); if (!applied && reason) put(result, "failureReason", str(reason));
    gboolean ok = reply(c, a->id, result, 0, NULL); g_hash_table_remove(c->applies, &token); return ok;
}
void ed_lsp_shutdown(EdLsp *c) {
    if (c->state == ED_LSP_EXITED || c->state == ED_LSP_FAILED || c->state == ED_LSP_STOPPING) return;
    if (c->state == ED_LSP_STARTING) { abort_client(c, _("Stopped before initialization completed")); return; }
    /* Retire every request before asking the server to shut down. */
    GArray *apply_tokens = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, c->applies);
    while (g_hash_table_iter_next(&it, NULL, &value)) { ApplyRequest *a = value; g_array_append_val(apply_tokens, a->token); }
    for (guint i = 0; i < apply_tokens->len; i++) ed_lsp_reply_apply_edit(c, g_array_index(apply_tokens, uint64_t, i), FALSE, "Editor is shutting down", NULL);
    g_array_unref(apply_tokens);
    for (GList *l = c->events.head; l;) {
        GList *next = l->next; EdLspEvent *e = l->data;
        if (e->kind == ED_LSP_EVENT_APPLY_EDIT) { g_queue_delete_link(&c->events, l); ed_lsp_event_free(e); }
        l = next;
    }
    g_hash_table_iter_init(&it, c->pending);
    while (g_hash_table_iter_next(&it, NULL, &value)) { Pending *p = value; result_event(c, p, ED_LSP_CANCELLED, _("Server shutting down"), 0, NULL); g_hash_table_iter_remove(&it); }
    state_event(c, ED_LSP_STOPPING, _("Language server shutting down")); c->stop_deadline = now_ms() + c->shutdown_ms;
    request_node(c, INTERNAL_SHUTDOWN, "shutdown", null_node(), NULL);
}
static void deadlines(EdLsp *c) {
    int64_t now = now_ms(); GArray *expired = g_array_new(FALSE, FALSE, sizeof(uint64_t)); GHashTableIter it; gpointer value;
    g_hash_table_iter_init(&it, c->pending); while (g_hash_table_iter_next(&it, NULL, &value)) { Pending *p = value; if (now >= p->deadline) g_array_append_val(expired, p->id); }
    for (guint i = 0; i < expired->len; i++) {
        uint64_t id = g_array_index(expired, uint64_t, i); Pending *p = g_hash_table_lookup(c->pending, &id); if (!p) continue; g_hash_table_steal(c->pending, &id);
        if (p->method == INTERNAL_INITIALIZE) abort_client(c, _("Language server initialization timed out"));
        else if (p->method >= 0) { send_cancel(c, id); result_event(c, p, ED_LSP_TIMEOUT, _("Language server request timed out"), 0, NULL); }
        pending_free(p);
    }
    g_array_unref(expired); expired = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    g_hash_table_iter_init(&it, c->applies); while (g_hash_table_iter_next(&it, NULL, &value)) { ApplyRequest *a = value; if (now >= a->deadline) g_array_append_val(expired, a->token); }
    for (guint i = 0; i < expired->len; i++) ed_lsp_reply_apply_edit(c, g_array_index(expired, uint64_t, i), FALSE, "Editor did not accept the edit before timeout", NULL);
    g_array_unref(expired);
    if (c->state == ED_LSP_STOPPING && now >= c->stop_deadline && c->pid > 0) {
        if (!c->kill_stage) { kill(-c->pid, SIGTERM); kill(c->pid, SIGTERM); c->kill_stage = 1; c->stop_deadline = now + 250; }
        else { kill(-c->pid, SIGKILL); kill(c->pid, SIGKILL); c->kill_stage = 2; c->stop_deadline = now + 250; }
    }
}
void ed_lsp_step(EdLsp *c, unsigned wait_ms) {
    if (!c) return;
    if (c->overflow) {
        g_queue_clear_full(&c->events, (GDestroyNotify)ed_lsp_event_free);
        abort_client(c, _("LSP event queue limit exceeded")); c->overflow = FALSE;
    }
    deadlines(c);
    int timeout = (int)MIN(wait_ms, 1000U); int64_t now = now_ms(); GHashTableIter it; gpointer value;
    g_hash_table_iter_init(&it, c->pending); while (g_hash_table_iter_next(&it, NULL, &value)) timeout = MIN(timeout, (int)MAX(0, ((Pending *)value)->deadline - now));
    g_hash_table_iter_init(&it, c->applies); while (g_hash_table_iter_next(&it, NULL, &value)) timeout = MIN(timeout, (int)MAX(0, ((ApplyRequest *)value)->deadline - now));
    if (c->state == ED_LSP_STOPPING) timeout = MIN(timeout, (int)MAX(0, c->stop_deadline - now));
    if (!g_queue_is_empty(&c->events) || c->state == ED_LSP_FAILED || c->state == ED_LSP_EXITED) timeout = 0;
    struct pollfd fds[3] = {{c->out_fd, POLLIN, 0}, {c->err_fd, POLLIN, 0}, {c->in_fd, c->output->len > c->written ? POLLOUT : 0, 0}};
    int ready = poll(fds, 3, timeout); if (ready < 0 && errno != EINTR) abort_client(c, _("Polling language server failed"));
    for (int channel = 0; channel < 2; channel++) {
        int *fd = channel ? &c->err_fd : &c->out_fd;
        if (*fd < 0 || !(fds[channel].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        /* A noisy server cannot monopolize the UI thread; continue next step. */
        size_t budget = 256 * 1024;
        while (budget && *fd >= 0) {
            guint8 buffer[8192]; ssize_t n = read(*fd, buffer, sizeof(buffer));
            if (n > 0) {
                budget -= MIN(budget, (size_t)n);
                if (channel) { g_string_append_len(c->stderr_text, (char *)buffer, n); if (c->stderr_text->len > STDERR_LIMIT) g_string_erase(c->stderr_text, 0, c->stderr_text->len - STDERR_LIMIT); }
                else { g_byte_array_append(c->input, buffer, (guint)n); consume_input(c); }
            } else if (!n) { close_fd(fd); break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { close_fd(fd); break; }
        }
    }
    if (c->in_fd >= 0 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP)) && c->output->len > c->written) {
        ssize_t n = pipe_write(c->in_fd, c->output->data + c->written, MIN(c->output->len - c->written, 256 * 1024));
        if (n > 0) c->written += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) abort_client(c, _("Language server stdin closed"));
        if (c->written == c->output->len) { g_byte_array_set_size(c->output, 0); c->written = 0; }
    }
    if (c->pid > 0) {
        int status; pid_t pid = waitpid(c->pid, &status, WNOHANG);
        if (pid > 0 || (pid < 0 && errno == ECHILD)) {
            gboolean normal = c->state == ED_LSP_STOPPING; g_spawn_close_pid(c->pid); c->pid = 0;
            if (normal) { close_fd(&c->in_fd); close_fd(&c->out_fd); close_fd(&c->err_fd); state_event(c, ED_LSP_EXITED, _("Language server exited")); }
            else abort_client(c, _("Language server exited unexpectedly"));
        }
    }
    if (c->out_fd < 0 && c->pid > 0 && c->state != ED_LSP_STOPPING) abort_client(c, _("Language server stdout closed"));
    deadlines(c);
}
static gpointer reap_child(gpointer data) {
    GPid pid = GPOINTER_TO_INT(data); int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    g_spawn_close_pid(pid); return NULL;
}
void ed_lsp_free(EdLsp *c) {
    if (!c) return;
    close_fd(&c->in_fd); close_fd(&c->out_fd); close_fd(&c->err_fd);
    if (c->pid > 0) {
        kill(-c->pid, SIGKILL); kill(c->pid, SIGKILL);
        int status; pid_t done = waitpid(c->pid, &status, WNOHANG);
        if (!done) {
            /* Even SIGKILL can wait on kernel I/O. Reap off the UI thread, with
             * no references back to the destroyed client. Normal shutdown is
             * reaped synchronously by step(), so this is the failure path only. */
            GThread *thread = g_thread_new("edit-lsp-reap", reap_child, GINT_TO_POINTER(c->pid)); g_thread_unref(thread);
        } else g_spawn_close_pid(c->pid);
    }
    g_hash_table_unref(c->documents); g_hash_table_unref(c->pending); g_hash_table_unref(c->applies);
    g_queue_clear_full(&c->events, (GDestroyNotify)ed_lsp_event_free); g_byte_array_unref(c->input); g_byte_array_unref(c->output); g_string_free(c->stderr_text, TRUE);
    if (c->settings) json_node_free(c->settings);
    g_free(c->caps.completion_triggers); g_free(c->caps.signature_triggers); g_free(c->caps.signature_retriggers); g_free(c->caps.format_triggers); g_free(c->caps.server_name); g_free(c->caps.server_version); g_free(c);
}
