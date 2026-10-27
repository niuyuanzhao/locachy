/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../base/include/base.h"
#include "i18n-stub.h"
#include "../progs/edit-lsp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *server_script;
static const char *uri_a = "file:///tmp/edit-lsp-a.c";
static const char *uri_b = "file:///tmp/edit-lsp-b.c";
static int64_t deadline(unsigned ms) { return g_get_monotonic_time() / 1000 + ms; }
static gboolean before(int64_t d) { return g_get_monotonic_time() / 1000 < d; }
static void drain(EdLsp *c) { EdLspEvent *e; while ((e = ed_lsp_next_event(c))) ed_lsp_event_free(e); }
static void wait_state(EdLsp *c, EdLspState target) {
    int64_t end = deadline(8000);
    while (before(end) && ed_lsp_state(c) != target && ed_lsp_state(c) != ED_LSP_FAILED) { ed_lsp_step(c, 10); drain(c); }
    if (ed_lsp_state(c) != target) g_printerr("Server state %d, wanted %d; stderr: %s\n", ed_lsp_state(c), target, ed_lsp_stderr(c));
    g_assert_cmpint(ed_lsp_state(c), ==, target);
}
static EdLspEvent *wait_id(EdLsp *c, uint64_t id, EdLspStatus status) {
    int64_t end = deadline(8000); EdLspEvent *e;
    while (before(end)) {
        ed_lsp_step(c, 10);
        while ((e = ed_lsp_next_event(c))) {
            if (e->kind == ED_LSP_EVENT_RESULT && e->id == id) {
                if (e->status != status) g_printerr("Request %" G_GUINT64_FORMAT ": %d wanted %d: %s; stderr: %s\n", id, e->status, status, e->message, ed_lsp_stderr(c));
                g_assert_cmpint(e->status, ==, status); return e;
            }
            ed_lsp_event_free(e);
        }
        g_assert_cmpint(ed_lsp_state(c), !=, ED_LSP_FAILED);
    }
    g_error("Timed out waiting for request %" G_GUINT64_FORMAT "; stderr: %s", id, ed_lsp_stderr(c)); return NULL;
}
static EdLsp *fake(const char *mode, unsigned timeout) {
    const char *argv[] = {"python3", server_script, mode, NULL};
    EdLspConfig config = {.argv = argv, .request_timeout_ms = timeout, .shutdown_timeout_ms = 80, .settings_json = "{\"test\":{\"flag\":true}}"};
    GError *error = NULL; EdLsp *c = ed_lsp_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(c); return c;
}
static void stop(EdLsp *c) { ed_lsp_shutdown(c); wait_state(c, ED_LSP_EXITED); ed_lsp_free(c); }
static void test_positions(void) {
    const char *s = "a😀中\r\nβ\rX\n"; const size_t offsets[] = {0, 1, 5, 8, 10, 12, 13, 14, 15};
    const EdLspPosition expected[] = {{0, 0}, {0, 1}, {0, 3}, {0, 4}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {3, 0}};
    for (guint i = 0; i < G_N_ELEMENTS(offsets); i++) {
        EdLspPosition p; size_t o; g_assert_true(ed_lsp_utf8_position(s, offsets[i], &p));
        g_assert_cmpuint(p.line, ==, expected[i].line); g_assert_cmpuint(p.character, ==, expected[i].character);
        g_assert_true(ed_lsp_position_utf8(s, p, &o)); g_assert_cmpuint(o, ==, offsets[i]);
    }
    EdLspPosition p; size_t o;
    g_assert_false(ed_lsp_utf8_position(s, 2, &p)); g_assert_false(ed_lsp_utf8_position(s, 9, &p));
    g_assert_false(ed_lsp_utf8_position(s, 100, &p)); g_assert_false(ed_lsp_utf8_position("\xff", 0, &p));
    g_assert_false(ed_lsp_position_utf8(s, (EdLspPosition){0, 2}, &o));
    g_assert_false(ed_lsp_position_utf8(s, (EdLspPosition){0, 99}, &o));
    g_assert_false(ed_lsp_position_utf8(s, (EdLspPosition){4, 0}, &o));
}
static void test_apply(void) {
    EdLspTextEdit a = {.range = {{0, 1}, {0, 3}}, .new_text = "X"};
    EdLspTextEdit b = {.range = {{1, 0}, {1, 0}}, .new_text = "first"};
    EdLspTextEdit c = {.range = {{1, 0}, {1, 0}}, .new_text = "second"};
    EdLspDocumentEdit d = {.version = 3, .edits = g_ptr_array_new()};
    g_ptr_array_add(d.edits, &a); g_ptr_array_add(d.edits, &b); g_ptr_array_add(d.edits, &c);
    GError *error = NULL; char *out = ed_lsp_apply_text_edits("a😀中\nβ", 3, &d, &error); g_assert_no_error(error); g_assert_cmpstr(out, ==, "aX中\nfirstsecondβ"); g_free(out);
    out = ed_lsp_apply_text_edits("a😀中\nβ", 4, &d, &error); g_assert_null(out); g_clear_error(&error);
    a.range.end.character = 2; out = ed_lsp_apply_text_edits("a😀中\nβ", 3, &d, &error); g_assert_null(out); g_clear_error(&error);
    a.range.end.character = 4; b.range = (EdLspRange){{0, 3}, {0, 4}}; out = ed_lsp_apply_text_edits("a😀中\nβ", 3, &d, &error); g_assert_null(out); g_clear_error(&error);
    g_ptr_array_unref(d.edits);
}
static void test_lifecycle(void) {
    EdLsp *c = fake("normal", 1500); GError *error = NULL;
    /* Pre-initialize edits are folded into didOpen, not sent too early. */
    g_assert_true(ed_lsp_open(c, uri_a, "c", "a😀中\r\nβ", 1, &error));
    g_assert_true(ed_lsp_change(c, uri_a, 1, 5, "🦀", 2, &error));
    g_assert_true(ed_lsp_open(c, uri_b, "c", "beta", 7, &error)); g_assert_no_error(error);
    wait_state(c, ED_LSP_READY);
    const EdLspCapabilities *caps = ed_lsp_capabilities(c);
    g_assert_true(caps->completion); g_assert_true(caps->prepare_rename); g_assert_cmpstr(caps->completion_triggers, ==, ".:"); g_assert_cmpint(caps->sync_kind, ==, 2);
    g_assert_true(ed_lsp_change(c, uri_a, 1, 5, "😀", 3, &error));
    g_assert_true(ed_lsp_save(c, uri_a, &error)); g_assert_no_error(error);
    gboolean saw_diag = FALSE, saw_save = FALSE; int64_t end = deadline(3000);
    while (before(end) && (!saw_diag || !saw_save)) {
        ed_lsp_step(c, 10); EdLspEvent *e;
        while ((e = ed_lsp_next_event(c))) {
            if (e->kind == ED_LSP_EVENT_DIAGNOSTICS && !g_strcmp0(e->uri, uri_a) && e->diagnostics->len) {
                g_assert_cmpint(e->version, ==, 3); EdLspDiagnostic *d = g_ptr_array_index(e->diagnostics, 0);
                g_assert_cmpstr(d->code, ==, "42"); g_assert_true(d->unnecessary); g_assert_cmpuint(d->related->len, ==, 1); saw_diag = TRUE;
            }
            if (e->kind == ED_LSP_EVENT_MESSAGE && !g_strcmp0(e->message, "saved a😀中\r\nβ")) saw_save = TRUE;
            ed_lsp_event_free(e);
        }
    }
    g_assert_true(saw_diag); g_assert_true(saw_save); g_assert_cmpuint(strlen(ed_lsp_stderr(c)), <=, 16384); g_assert_cmpuint(strlen(ed_lsp_stderr(c)), >, 10000);
    g_assert_false(ed_lsp_change(c, uri_a, 2, 3, "x", 4, &error)); g_clear_error(&error);
    g_assert_false(ed_lsp_change(c, uri_a, 0, 0, "x", 3, &error)); g_clear_error(&error);
    int64_t version; g_assert_cmpstr(ed_lsp_document(c, uri_a, &version), ==, "a😀中\r\nβ"); g_assert_cmpint(version, ==, 3);
    g_assert_true(ed_lsp_close(c, uri_a, &error)); g_assert_true(ed_lsp_close(c, uri_b, &error)); g_assert_no_error(error);
    for (int i = 0; i < 20; i++) { ed_lsp_step(c, 1); EdLspEvent *e; while ((e = ed_lsp_next_event(c))) { if (e->kind == ED_LSP_EVENT_DIAGNOSTICS) g_assert_cmpuint(e->diagnostics->len, ==, 0); ed_lsp_event_free(e); } }
    stop(c);
}
static void test_operations(void) {
    EdLsp *c = fake("normal", 2000); GError *error = NULL;
    g_assert_true(ed_lsp_open(c, uri_a, "c", "alpha", 1, &error)); g_assert_true(ed_lsp_open(c, uri_b, "c", "beta", 1, &error)); wait_state(c, ED_LSP_READY);
    for (int method = ED_LSP_COMPLETION; method <= ED_LSP_SWITCH_SOURCE_HEADER; method++) {
        if (method == ED_LSP_COMPLETION_RESOLVE || method == ED_LSP_CODE_ACTION_RESOLVE || method == ED_LSP_EXECUTE_COMMAND) continue;
        EdLspRequest r = {.method = (EdLspMethod)method, .uri = uri_a, .offset = 0, .end_offset = 1, .text = "renamed", .include_declaration = TRUE, .insert_spaces = TRUE};
        EdLspDiagnostic diag = {.payload_json = "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},\"message\":\"x\",\"data\":{\"fix\":1}}"};
        if (method == ED_LSP_CODE_ACTION) { r.diagnostics = g_ptr_array_new(); g_ptr_array_add(r.diagnostics, &diag); r.only_kind = "quickfix"; }
        uint64_t id = ed_lsp_request(c, &r, &error); g_assert_no_error(error); g_assert_cmpuint(id, >, 0);
        if (r.diagnostics) g_ptr_array_unref(r.diagnostics);
        EdLspEvent *e = wait_id(c, id, ED_LSP_OK); EdLspResult *out = e->result; g_assert_cmpint(out->method, ==, method);
        if (method == ED_LSP_COMPLETION) {
            g_assert_true(out->incomplete); g_assert_cmpuint(out->items->len, ==, 1); EdLspCompletion *item = g_ptr_array_index(out->items, 0); g_assert_cmpstr(item->label, ==, "alpha"); g_assert_true(item->has_insert_range); g_assert_cmpuint(item->additional_edits->len, ==, 1);
            EdLspRequest resolve = {.method = ED_LSP_COMPLETION_RESOLVE, .uri = uri_a, .payload_json = item->payload_json}; uint64_t rid = ed_lsp_request(c, &resolve, &error); g_assert_no_error(error);
            EdLspEvent *resolved = wait_id(c, rid, ED_LSP_OK); EdLspCompletion *ri = g_ptr_array_index(resolved->result->items, 0); g_assert_cmpstr(ri->documentation, ==, "Resolved **help**"); ed_lsp_event_free(resolved);
        } else if (method == ED_LSP_SIGNATURE) {
            g_assert_cmpint(out->active_parameter, ==, 1); EdLspSignature *s = g_ptr_array_index(out->items, 0); g_assert_cmpuint(s->parameters->len, ==, 2); EdLspParameter *p = g_ptr_array_index(s->parameters, 0); g_assert_true(p->has_label_offsets); g_assert_cmpuint(p->label_end, ==, 11);
        } else if (method == ED_LSP_HOVER) { g_assert_true(out->markdown); g_assert_cmpstr(out->text, ==, "**alpha** 世界"); }
        else if (method >= ED_LSP_DEFINITION && method <= ED_LSP_IMPLEMENTATION) { EdLspLocation *l = g_ptr_array_index(out->items, 0); g_assert_true(l->has_origin); g_assert_cmpstr(l->uri, ==, uri_a); }
        else if (method == ED_LSP_REFERENCES) g_assert_cmpuint(out->items->len, ==, 2);
        else if (method == ED_LSP_DOCUMENT_SYMBOLS) { EdLspSymbol *s = g_ptr_array_index(out->items, 0); g_assert_cmpuint(s->children->len, ==, 1); }
        else if (method == ED_LSP_CODE_ACTION) {
            EdLspCodeAction *a = g_ptr_array_index(out->items, 0); g_assert_true(a->preferred); g_assert_nonnull(a->edit); g_assert_cmpstr(a->command, ==, "fix");
            EdLspRequest resolve = {.method = ED_LSP_CODE_ACTION_RESOLVE, .uri = uri_a, .payload_json = a->payload_json}; uint64_t rid = ed_lsp_request(c, &resolve, &error); g_assert_no_error(error);
            EdLspEvent *resolved = wait_id(c, rid, ED_LSP_OK); EdLspCodeAction *ra = g_ptr_array_index(resolved->result->items, 0); g_assert_nonnull(ra->edit); ed_lsp_event_free(resolved);
        } else if (method == ED_LSP_RENAME || method == ED_LSP_FORMAT || method == ED_LSP_RANGE_FORMAT || method == ED_LSP_ON_TYPE_FORMAT) {
            g_assert_nonnull(out->edit); EdLspDocumentEdit *d = g_ptr_array_index(out->edit->documents, 0); g_assert_cmpint(d->version, ==, 1);
            char *text = ed_lsp_apply_text_edits("alpha", 1, d, &error); g_assert_no_error(error); g_assert_nonnull(text); g_free(text);
        } else if (method == ED_LSP_SWITCH_SOURCE_HEADER) g_assert_cmpstr(out->text, ==, "file:///tmp/header.h");
        ed_lsp_event_free(e);
    }
    EdLspRequest execute = {.method = ED_LSP_EXECUTE_COMMAND, .command = "apply", .arguments_json = "[]"};
    uint64_t id = ed_lsp_request(c, &execute, &error); g_assert_no_error(error); g_assert_cmpuint(id, >, 0);
    gboolean applied = FALSE, acknowledged = FALSE, completed = FALSE; int64_t end = deadline(3000);
    while (before(end) && (!applied || !acknowledged || !completed)) {
        ed_lsp_step(c, 10); EdLspEvent *e;
        while ((e = ed_lsp_next_event(c))) {
            if (e->kind == ED_LSP_EVENT_APPLY_EDIT) {
                g_assert_cmpuint(e->edit->documents->len, ==, 4); g_assert_cmpuint(e->edit->annotations->len, ==, 1);
                EdLspDocumentEdit *d = g_ptr_array_index(e->edit->documents, 2); g_assert_cmpint(d->kind, ==, ED_LSP_EDIT_RENAME); g_assert_true(d->overwrite);
                g_assert_true(ed_lsp_reply_apply_edit(c, e->id, TRUE, NULL, &error)); g_assert_no_error(error); applied = TRUE;
            } else if (e->kind == ED_LSP_EVENT_MESSAGE && !g_strcmp0(e->message, "apply acknowledged")) acknowledged = TRUE;
            else if (e->kind == ED_LSP_EVENT_RESULT && e->id == id) { g_assert_cmpint(e->status, ==, ED_LSP_OK); completed = TRUE; }
            ed_lsp_event_free(e);
        }
    }
    g_assert_true(applied); g_assert_true(acknowledged); g_assert_true(completed);
    execute.command = "unknown"; id = ed_lsp_request(c, &execute, &error); g_assert_no_error(error); EdLspEvent *e = wait_id(c, id, ED_LSP_SERVER_ERROR); g_assert_cmpint(e->error_code, ==, -32602); ed_lsp_event_free(e);
    stop(c);
}
/* A logMessage the fake server sends after a command, other events dropped. */
static void wait_message(EdLsp *c, const char *message) {
    int64_t end = deadline(3000); gboolean seen = FALSE;
    while (before(end) && !seen) {
        ed_lsp_step(c, 10); EdLspEvent *e;
        while ((e = ed_lsp_next_event(c))) {
            if (e->kind == ED_LSP_EVENT_MESSAGE && !seen) { if (strcmp(e->message, message) && !g_str_has_prefix(e->message, "saved")) g_printerr("message: %s\n", e->message); seen = !strcmp(e->message, message); }
            ed_lsp_event_free(e);
        }
    }
    g_assert_true(seen);
}
static void check_resource(EdLspDocumentEdit *d, EdLspEditKind kind, const char *uri, const char *old_uri, gboolean overwrite, gboolean ignore_if_exists, gboolean recursive, gboolean ignore_if_not_exists) {
    g_assert_cmpint(d->kind, ==, kind); g_assert_cmpstr(d->uri, ==, uri); if (old_uri) g_assert_cmpstr(d->old_uri, ==, old_uri);
    g_assert_cmpint(d->overwrite, ==, overwrite); g_assert_cmpint(d->ignore_if_exists, ==, ignore_if_exists);
    g_assert_cmpint(d->recursive, ==, recursive); g_assert_cmpint(d->ignore_if_not_exists, ==, ignore_if_not_exists);
    g_assert_cmpint(d->version, ==, -1); g_assert_cmpuint(d->edits->len, ==, 0);
}
/* CreateFile/RenameFile/DeleteFile with each option, malformed operations,
 * the advertised workspace edit capability, and the applied=false reply. */
static void test_resource_operations(void) {
    EdLsp *c = fake("normal", 2000); GError *error = NULL;
    g_assert_true(ed_lsp_open(c, uri_a, "c", "alpha", 1, &error)); wait_state(c, ED_LSP_READY);
    EdLspRequest r = {.method = ED_LSP_EXECUTE_COMMAND, .command = "capabilities"};
    g_assert_cmpuint(ed_lsp_request(c, &r, &error), >, 0); g_assert_no_error(error);
    wait_message(c, "{\"applyEdit\": true, \"configuration\": true, \"symbol\": {}, \"workspaceEdit\": {\"changeAnnotationSupport\": {}, "
                    "\"documentChanges\": true, \"failureHandling\": \"undo\", \"resourceOperations\": [\"create\", \"rename\", \"delete\"]}}");
    r.command = "apply-resources"; g_assert_cmpuint(ed_lsp_request(c, &r, &error), >, 0); g_assert_no_error(error);
    EdLspEvent *apply = NULL; int64_t end = deadline(3000);
    while (before(end) && !apply) { ed_lsp_step(c, 10); EdLspEvent *e; while ((e = ed_lsp_next_event(c))) { if (e->kind == ED_LSP_EVENT_APPLY_EDIT && !apply) apply = e; else ed_lsp_event_free(e); } }
    g_assert_nonnull(apply); g_assert_cmpstr(apply->message, ==, "Resources");
    GPtrArray *d = apply->edit->documents; g_assert_cmpuint(d->len, ==, 14);
#define AT(i) ((EdLspDocumentEdit *)g_ptr_array_index(d, i))
    check_resource(AT(0), ED_LSP_EDIT_CREATE, "file:///tmp/c1.c", NULL, FALSE, FALSE, FALSE, FALSE);
    check_resource(AT(1), ED_LSP_EDIT_CREATE, "file:///tmp/c2.c", NULL, TRUE, TRUE, FALSE, FALSE); g_assert_cmpstr(AT(1)->annotation_id, ==, "a");
    check_resource(AT(2), ED_LSP_EDIT_CREATE, "file:///tmp/c3.c", NULL, FALSE, TRUE, FALSE, FALSE);
    g_assert_cmpint(AT(3)->kind, ==, ED_LSP_EDIT_TEXT); g_assert_cmpint(AT(3)->version, ==, -1); g_assert_cmpuint(AT(3)->edits->len, ==, 1);
    check_resource(AT(4), ED_LSP_EDIT_RENAME, "file:///tmp/r2.c", "file:///tmp/r1.c", FALSE, FALSE, FALSE, FALSE);
    check_resource(AT(5), ED_LSP_EDIT_RENAME, "file:///tmp/r4.c", "file:///tmp/r3.c", FALSE, TRUE, FALSE, FALSE);
    check_resource(AT(6), ED_LSP_EDIT_DELETE, "file:///tmp/d1", NULL, FALSE, FALSE, TRUE, FALSE);
    check_resource(AT(7), ED_LSP_EDIT_DELETE, "file:///tmp/d2.c", NULL, FALSE, FALSE, FALSE, TRUE);
    for (guint i = 8; i < 13; i++) g_assert_cmpint(AT(i)->kind, ==, ED_LSP_EDIT_INVALID);
    g_assert_cmpint(AT(13)->kind, ==, ED_LSP_EDIT_TEXT); g_assert_cmpint(AT(13)->version, ==, 1);
    /* Resource operations are not text: the pure helper refuses them. */
    char *text = ed_lsp_apply_text_edits("alpha", 1, AT(0), &error); g_assert_null(text); g_assert_nonnull(error); g_clear_error(&error);
#undef AT
    g_assert_cmpuint(apply->edit->annotations->len, ==, 1);
    g_assert_true(ed_lsp_reply_apply_edit(c, apply->id, FALSE, "rolled back: test", &error)); g_assert_no_error(error);
    g_assert_false(ed_lsp_reply_apply_edit(c, apply->id, TRUE, NULL, &error)); g_clear_error(&error); /* one reply only */
    ed_lsp_event_free(apply); wait_message(c, "apply-resources failed: rolled back: test");
    stop(c);
}
static void test_cancel_stale_timeout(void) {
    EdLsp *c = fake("normal", 1000); GError *error = NULL;
    g_assert_true(ed_lsp_open(c, uri_a, "c", "alpha", 1, &error)); wait_state(c, ED_LSP_READY);
    EdLspRequest r = {.method = ED_LSP_EXECUTE_COMMAND, .uri = uri_a, .command = "hold"};
    uint64_t id = ed_lsp_request(c, &r, &error); g_assert_no_error(error); ed_lsp_step(c, 10); ed_lsp_cancel(c, id);
    EdLspEvent *e = wait_id(c, id, ED_LSP_CANCELLED); ed_lsp_event_free(e);
    id = ed_lsp_request(c, &r, &error); g_assert_no_error(error); ed_lsp_step(c, 10);
    g_assert_true(ed_lsp_change(c, uri_a, 0, 0, "z", 2, &error)); g_assert_no_error(error); e = wait_id(c, id, ED_LSP_STALE); ed_lsp_event_free(e);
    id = ed_lsp_request(c, &r, &error); g_assert_no_error(error); e = wait_id(c, id, ED_LSP_TIMEOUT); ed_lsp_event_free(e);
    /* A queued response, not only an outstanding request, must become stale. */
    r = (EdLspRequest){.method = ED_LSP_HOVER, .uri = uri_a}; id = ed_lsp_request(c, &r, &error); g_assert_no_error(error);
    for (int i = 0; i < 20; i++) ed_lsp_step(c, 1);
    g_assert_true(ed_lsp_change(c, uri_a, 0, 0, "y", 3, &error)); g_assert_no_error(error); e = wait_id(c, id, ED_LSP_STALE); ed_lsp_event_free(e);
    stop(c);
}
static void test_failures(void) {
    const char *modes[] = {"oversize", "malformed", "duplicate-header", "init-hang", "partial-crash", "nul"};
    for (guint i = 0; i < G_N_ELEMENTS(modes); i++) { EdLsp *c = fake(modes[i], 150); wait_state(c, ED_LSP_FAILED); ed_lsp_free(c); }
    EdLsp *c = fake("shutdown-hang", 2000); wait_state(c, ED_LSP_READY); int64_t start = deadline(0); stop(c); g_assert_cmpint(deadline(0) - start, <, 1500);
    c = fake("normal", 2000); wait_state(c, ED_LSP_READY); GError *error = NULL;
    EdLspRequest r = {.method = ED_LSP_EXECUTE_COMMAND, .command = "crash"}; g_assert_cmpuint(ed_lsp_request(c, &r, &error), >, 0); g_assert_no_error(error); wait_state(c, ED_LSP_FAILED); ed_lsp_free(c);
}
static void test_limits_and_apply_timeout(void) {
    EdLsp *c = fake("flood", 2000); int64_t end = deadline(4000);
    /* Deliberately do not drain events: the client must fail closed, not grow. */
    while (before(end) && ed_lsp_state(c) != ED_LSP_FAILED) ed_lsp_step(c, 1);
    g_assert_cmpint(ed_lsp_state(c), ==, ED_LSP_FAILED); ed_lsp_free(c);
    c = fake("normal", 200); GError *error = NULL; g_assert_true(ed_lsp_open(c, uri_a, "c", "alpha", 1, &error)); wait_state(c, ED_LSP_READY);
    EdLspRequest r = {.method = ED_LSP_EXECUTE_COMMAND, .command = "apply-timeout"};
    g_assert_cmpuint(ed_lsp_request(c, &r, &error), >, 0); g_assert_no_error(error);
    gboolean expired = FALSE; uint64_t token = 0; end = deadline(3000);
    while (before(end) && !expired) {
        ed_lsp_step(c, 10); EdLspEvent *e;
        while ((e = ed_lsp_next_event(c))) {
            if (e->kind == ED_LSP_EVENT_APPLY_EDIT) token = e->id;
            expired |= e->kind == ED_LSP_EVENT_MESSAGE && !g_strcmp0(e->message, "apply expired"); ed_lsp_event_free(e);
        }
    }
    g_assert_true(expired); g_assert_cmpuint(token, >, 0); g_assert_false(ed_lsp_reply_apply_edit(c, token, TRUE, NULL, &error)); g_clear_error(&error); stop(c);
    const char *bad_argv[] = {"/nonexistent/edit-lsp-server", NULL}; EdLspConfig bad = {.argv = bad_argv};
    c = ed_lsp_new(&bad, &error); g_assert_null(c); g_assert_nonnull(error); g_clear_error(&error);
    bad.initialization_options_json = "{"; c = ed_lsp_new(&bad, &error); g_assert_null(c); g_assert_nonnull(error); g_clear_error(&error);
}
static void test_slow_fragments(void) {
    EdLsp *c = fake("slow-fragments", 5000); GError *error = NULL;
    g_assert_true(ed_lsp_open(c, uri_a, "c", "a😀中", 1, &error)); g_assert_no_error(error); wait_state(c, ED_LSP_READY);
    EdLspRequest r = {.method = ED_LSP_HOVER, .uri = uri_a, .offset = 5}; uint64_t id = ed_lsp_request(c, &r, &error); g_assert_no_error(error);
    EdLspEvent *e = wait_id(c, id, ED_LSP_OK); g_assert_cmpstr(e->result->text, ==, "**alpha** 世界"); ed_lsp_event_free(e); stop(c);
}
static void test_full_sync(void) {
    EdLsp *c = fake("full", 1500); GError *error = NULL; g_assert_true(ed_lsp_open(c, uri_a, "c", "alpha", 1, &error)); wait_state(c, ED_LSP_READY);
    g_assert_true(ed_lsp_change(c, uri_a, 0, 1, "A", 2, &error)); g_assert_true(ed_lsp_save(c, uri_a, &error)); g_assert_no_error(error);
    gboolean saved = FALSE; int64_t end = deadline(3000);
    while (before(end) && !saved) { ed_lsp_step(c, 10); EdLspEvent *e; while ((e = ed_lsp_next_event(c))) { saved |= e->kind == ED_LSP_EVENT_MESSAGE && !g_strcmp0(e->message, "saved Alpha"); ed_lsp_event_free(e); } }
    g_assert_true(saved); stop(c);
}
static EdLspEvent *clang_request(EdLsp *c, EdLspMethod method, const char *uri, size_t offset, size_t end, const char *text, GPtrArray *diags) {
    EdLspRequest r = {.method = method, .uri = uri, .offset = offset, .end_offset = end, .text = text, .include_declaration = TRUE, .insert_spaces = TRUE, .diagnostics = diags};
    GError *error = NULL; uint64_t id = ed_lsp_request(c, &r, &error); g_assert_no_error(error); g_assert_cmpuint(id, >, 0); return wait_id(c, id, ED_LSP_OK);
}
static void test_clangd(void) {
    char *dir = g_dir_make_tmp("edit-lsp-XXXXXX", NULL); g_assert_nonnull(dir);
    char *file = g_build_filename(dir, "sample.cpp", NULL), *uri = g_filename_to_uri(file, NULL, NULL), *root = g_filename_to_uri(dir, NULL, NULL);
    const char *code = "// Unicode 😀 中\nint add(int a, int b) { return a+b; }\nint main() { int value=add(1, 2); return value; }\n";
    g_assert_true(g_file_set_contents(file, code, -1, NULL));
    const char *argv[] = {"clangd", "--log=error", "--background-index=false", "--clang-tidy=false", NULL};
    EdLspConfig config = {.argv = argv, .cwd = dir, .root_uri = root, .request_timeout_ms = 10000, .shutdown_timeout_ms = 2000};
    GError *error = NULL; EdLsp *c = ed_lsp_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(c);
    g_assert_true(ed_lsp_open(c, uri, "cpp", code, 1, &error)); wait_state(c, ED_LSP_READY);
    size_t call = (size_t)(strstr(code, "add(1") - code), value = (size_t)(strstr(code, "value=") - code);
    EdLspEvent *e = clang_request(c, ED_LSP_HOVER, uri, call + 1, 0, NULL, NULL); g_assert_nonnull(strstr(e->result->text, "add")); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_DEFINITION, uri, call + 1, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >, 0); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_DECLARATION, uri, call + 1, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >, 0); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_REFERENCES, uri, call + 1, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >=, 2); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_DOCUMENT_SYMBOLS, uri, 0, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >=, 2); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_COMPLETION, uri, call + 2, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >, 0); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_SIGNATURE, uri, call + 7, 0, NULL, NULL); g_assert_cmpuint(e->result->items->len, >, 0); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_RENAME, uri, value + 2, 0, "renamed", NULL); g_assert_nonnull(e->result->edit); g_assert_cmpuint(e->result->edit->documents->len, >, 0);
    EdLspDocumentEdit *d = g_ptr_array_index(e->result->edit->documents, 0); char *renamed = ed_lsp_apply_text_edits(code, 1, d, &error); g_assert_no_error(error); g_assert_nonnull(strstr(renamed, "int renamed")); g_assert_nonnull(strstr(renamed, "return renamed")); g_free(renamed); ed_lsp_event_free(e);
    e = clang_request(c, ED_LSP_FORMAT, uri, 0, 0, NULL, NULL); g_assert_cmpuint(e->result->edit->documents->len, ==, 1); d = g_ptr_array_index(e->result->edit->documents, 0); g_assert_cmpuint(d->edits->len, >, 0); ed_lsp_event_free(e);
    /* Actual compiler diagnostics and a fix-it, not a fake language engine. */
    const char *broken = "int main() { int value = 1 return value; }\n";
    g_assert_true(ed_lsp_change(c, uri, 0, strlen(code), broken, 2, &error)); g_assert_true(ed_lsp_save(c, uri, &error)); g_assert_no_error(error);
    EdLspEvent *diags = NULL; int64_t end = deadline(8000);
    while (before(end) && !diags) { ed_lsp_step(c, 10); while ((e = ed_lsp_next_event(c))) { if (e->kind == ED_LSP_EVENT_DIAGNOSTICS && e->diagnostics->len && e->version == 2) { diags = e; break; } ed_lsp_event_free(e); } }
    g_assert_nonnull(diags); EdLspDiagnostic *diag = g_ptr_array_index(diags->diagnostics, 0); size_t a, b; g_assert_true(ed_lsp_position_utf8(broken, diag->range.start, &a)); g_assert_true(ed_lsp_position_utf8(broken, diag->range.end, &b));
    e = clang_request(c, ED_LSP_CODE_ACTION, uri, a, b, NULL, diags->diagnostics); g_assert_cmpuint(e->result->items->len, >, 0);
    gboolean found_fix = FALSE;
    for (guint i = 0; i < e->result->items->len; i++) { EdLspCodeAction *action = g_ptr_array_index(e->result->items, i); if (action->edit && action->edit->documents->len) { d = g_ptr_array_index(action->edit->documents, 0); char *fixed = ed_lsp_apply_text_edits(broken, 2, d, &error); g_assert_no_error(error); g_assert_nonnull(fixed); found_fix |= strstr(fixed, "1;") != NULL; g_free(fixed); } }
    g_assert_true(found_fix); ed_lsp_event_free(e); ed_lsp_event_free(diags);
    g_assert_true(ed_lsp_close(c, uri, &error)); g_assert_no_error(error); stop(c);
    unlink(file); rmdir(dir); g_free(file); g_free(uri); g_free(root); g_free(dir);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); server_script = g_getenv("EDIT_LSP_TEST_SERVER"); g_assert_nonnull(server_script);
    g_test_add_func("/edit-lsp/utf16", test_positions); g_test_add_func("/edit-lsp/workspace-edits", test_apply);
    g_test_add_func("/edit-lsp/lifecycle-fragments-multidoc", test_lifecycle); g_test_add_func("/edit-lsp/operations", test_operations);
    g_test_add_func("/edit-lsp/resource-operations", test_resource_operations);
    g_test_add_func("/edit-lsp/cancel-stale-timeout", test_cancel_stale_timeout); g_test_add_func("/edit-lsp/process-failures", test_failures);
    g_test_add_func("/edit-lsp/limits-apply-timeout", test_limits_and_apply_timeout);
    g_test_add_func("/edit-lsp/slow-fragments", test_slow_fragments);
    g_test_add_func("/edit-lsp/full-sync", test_full_sync); g_test_add_func("/edit-lsp/real-clangd", test_clangd);
    return g_test_run();
}
