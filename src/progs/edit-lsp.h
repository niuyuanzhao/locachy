/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOCACHY_EDIT_LSP_H
#define LOCACHY_EDIT_LSP_H

#include <glib.h>
#include <stdint.h>
#include <stddef.h>

/* Single-threaded, asynchronous stdio LSP client. No GLib main loop is required.
 * Every call is nonblocking except step(), whose wait is bounded by wait_ms.
 * One client represents one configured server/project, and may own many documents.
 * URIs must be absolute file: URIs (g_filename_to_uri is suitable). Offsets passed
 * to the request/change API are UTF-8 byte offsets in the client's current text.
 * Result ranges are LSP UTF-16; use the conversion helpers against the SAME version.
 * All event/result pointers belong to the event, and survive until event_free().
 * Do not apply edits automatically: validate versions and let the editor apply the
 * documentChanges in order (text edits one undo step per document, resource
 * operations as the user), undoing what it did if one fails ("undo" failure handling).
 */
typedef struct EdLsp EdLsp;
typedef struct { uint32_t line, character; } EdLspPosition;
typedef struct { EdLspPosition start, end; } EdLspRange;
typedef enum { ED_LSP_STARTING, ED_LSP_READY, ED_LSP_STOPPING,
               ED_LSP_EXITED, ED_LSP_FAILED } EdLspState;
typedef enum {
    ED_LSP_COMPLETION, ED_LSP_COMPLETION_RESOLVE, ED_LSP_SIGNATURE, ED_LSP_HOVER,
    ED_LSP_DEFINITION, ED_LSP_DECLARATION, ED_LSP_TYPE_DEFINITION,
    ED_LSP_IMPLEMENTATION, ED_LSP_REFERENCES, ED_LSP_DOCUMENT_SYMBOLS,
    ED_LSP_WORKSPACE_SYMBOLS, ED_LSP_HIGHLIGHT, ED_LSP_CODE_ACTION,
    ED_LSP_CODE_ACTION_RESOLVE, ED_LSP_RENAME, ED_LSP_PREPARE_RENAME,
    ED_LSP_FORMAT, ED_LSP_RANGE_FORMAT, ED_LSP_ON_TYPE_FORMAT,
    ED_LSP_EXECUTE_COMMAND, ED_LSP_SWITCH_SOURCE_HEADER
} EdLspMethod;
typedef enum { ED_LSP_OK, ED_LSP_CANCELLED, ED_LSP_STALE, ED_LSP_TIMEOUT,
               ED_LSP_SERVER_ERROR, ED_LSP_TRANSPORT_ERROR } EdLspStatus;

typedef struct {
    const char *const *argv;      /* argv[0] is searched in PATH; never a shell. */
    const char *cwd;              /* Optional working directory. */
    const char *root_uri;         /* Optional project file: URI. */
    const char *initialization_options_json; /* Optional JSON object/value. */
    const char *settings_json;    /* Optional workspace configuration object. */
    unsigned request_timeout_ms; /* 0 = 10000; includes initialization. */
    unsigned shutdown_timeout_ms;/* 0 = 1500; then TERM, then KILL after 250ms. */
    size_t max_message_bytes;    /* 0 = 16 MiB; inbound and outbound. */
} EdLspConfig;

typedef struct {
    gboolean completion, signature, hover, definition, declaration, type_definition;
    gboolean implementation, references, document_symbols, workspace_symbols;
    gboolean highlight, code_action, rename, prepare_rename, formatting;
    gboolean range_formatting, on_type_formatting, completion_resolve, action_resolve;
    int sync_kind;               /* 0 none, 1 full, 2 incremental. */
    char *completion_triggers, *signature_triggers, *signature_retriggers;
    char *format_triggers;       /* Concatenated Unicode trigger characters. */
    char *server_name, *server_version;
} EdLspCapabilities;

typedef struct {
    EdLspRange range;
    char *new_text, *annotation_id;
} EdLspTextEdit;
typedef enum { ED_LSP_EDIT_TEXT, ED_LSP_EDIT_CREATE, ED_LSP_EDIT_RENAME,
               ED_LSP_EDIT_DELETE, ED_LSP_EDIT_INVALID } EdLspEditKind;
typedef struct {
    EdLspEditKind kind;
    char *uri, *old_uri, *annotation_id; /* Rename: old_uri -> uri (its newUri). */
    int64_t version;             /* -1 = unspecified; -2 = malformed, reject. */
    gboolean overwrite, ignore_if_exists;       /* CreateFile/RenameFile options. */
    gboolean recursive, ignore_if_not_exists;   /* DeleteFile options. */
    GPtrArray *edits;            /* EdLspTextEdit*, ordered as sent by the server. */
} EdLspDocumentEdit;
typedef struct { char *id, *label, *description; gboolean needs_confirmation; } EdLspAnnotation;
typedef struct {
    GPtrArray *documents;        /* EdLspDocumentEdit*, documentChanges order preserved. */
    GPtrArray *annotations;      /* EdLspAnnotation*. */
} EdLspWorkspaceEdit;
typedef struct {
    char *uri;
    EdLspRange range, selection_range, origin_range;
    gboolean has_origin;
} EdLspLocation;
typedef struct EdLspSymbol {
    char *name, *detail, *container_name, *uri;
    int kind;
    gboolean deprecated;
    EdLspRange range, selection_range;
    GPtrArray *children;         /* EdLspSymbol*. */
} EdLspSymbol;
typedef struct {
    char *message, *source, *code, *code_uri;
    EdLspRange range;
    int severity;
    gboolean unnecessary, deprecated;
    GPtrArray *related;          /* EdLspLocation*; corresponding messages below. */
    GPtrArray *related_messages; /* char*. */
    char *payload_json;          /* Opaque original Diagnostic, for codeAction context. */
} EdLspDiagnostic;
typedef struct {
    char *label, *detail, *documentation, *sort_text, *filter_text, *insert_text;
    gboolean documentation_markdown, deprecated, preselect, snippet;
    int kind;
    gboolean has_edit, has_insert_range;
    EdLspRange edit_range, insert_range;
    GPtrArray *additional_edits; /* EdLspTextEdit*. */
    char *command, *command_arguments_json, *payload_json;
} EdLspCompletion;
typedef struct {
    char *label, *documentation;
    gboolean documentation_markdown, has_label_offsets;
    uint32_t label_start, label_end; /* UTF-16 offsets into signature label. */
} EdLspParameter;
typedef struct {
    char *label, *documentation;
    gboolean documentation_markdown;
    int active_parameter;        /* -1 = use SignatureHelp.activeParameter. */
    GPtrArray *parameters;       /* EdLspParameter*. */
} EdLspSignature;
typedef struct {
    char *title, *kind, *disabled_reason;
    gboolean preferred;
    EdLspWorkspaceEdit *edit;
    char *command, *arguments_json, *payload_json;
    GPtrArray *diagnostics;      /* EdLspDiagnostic*. */
} EdLspCodeAction;
typedef struct { EdLspRange range; int kind; } EdLspHighlight;

typedef struct {
    EdLspMethod method;
    GPtrArray *items;            /* completion/signature/location/symbol/action/highlight,
                                  according to method; NULL otherwise. */
    gboolean incomplete;        /* CompletionList.isIncomplete. */
    int active_signature, active_parameter;
    char *text;                 /* Hover text / prepareRename placeholder / header URI. */
    gboolean markdown, has_range;
    EdLspRange range;
    EdLspWorkspaceEdit *edit;    /* Rename and formatting results. */
} EdLspResult;
typedef enum { ED_LSP_EVENT_STATE, ED_LSP_EVENT_RESULT, ED_LSP_EVENT_DIAGNOSTICS,
               ED_LSP_EVENT_APPLY_EDIT, ED_LSP_EVENT_MESSAGE } EdLspEventKind;
typedef struct {
    EdLspEventKind kind;
    uint64_t id;
    EdLspState state;
    EdLspStatus status;
    int64_t version;            /* Document version, or -1 if unversioned. */
    uint64_t revision;          /* Client revision when generated; workspace edit guard. */
    char *uri, *message;
    int error_code;             /* JSON-RPC error code or message severity. */
    EdLspResult *result;
    GPtrArray *diagnostics;     /* EdLspDiagnostic*; empty array clears old diagnostics. */
    EdLspWorkspaceEdit *edit;   /* workspace/applyEdit; reply explicitly with the token. */
} EdLspEvent;

typedef struct {
    EdLspMethod method;
    const char *uri;            /* Required for textDocument requests. */
    size_t offset, end_offset;  /* codeAction/rangeFormatting use [offset,end_offset]. */
    const char *text;           /* Rename name / workspace symbol query / on-type char. */
    const char *trigger;        /* Optional completion/signature trigger character. */
    gboolean retrigger, include_declaration;
    unsigned tab_size;          /* Formatting: 0 = 4. */
    gboolean insert_spaces;
    GPtrArray *diagnostics;     /* Optional EdLspDiagnostic* for codeAction context. */
    const char *only_kind;      /* e.g. "quickfix"; NULL requests all code actions. */
    const char *payload_json;   /* Resolve: completion/action's opaque payload_json. */
    const char *command, *arguments_json; /* workspace/executeCommand. */
} EdLspRequest;

EdLsp *ed_lsp_new(const EdLspConfig *config, GError **error);
void ed_lsp_free(EdLsp *client); /* Immediate bounded cleanup; use shutdown first. */
EdLspState ed_lsp_state(const EdLsp *client);
const EdLspCapabilities *ed_lsp_capabilities(const EdLsp *client);
const char *ed_lsp_stderr(const EdLsp *client); /* Last <= 16 KiB, drained continuously. */
uint64_t ed_lsp_revision(const EdLsp *client);
void ed_lsp_step(EdLsp *client, unsigned wait_ms); /* Clamp to 1000ms and deadlines. */
EdLspEvent *ed_lsp_next_event(EdLsp *client);
void ed_lsp_event_free(EdLspEvent *event);
void ed_lsp_shutdown(EdLsp *client); /* Async shutdown -> exit -> bounded TERM/KILL. */

gboolean ed_lsp_open(EdLsp *, const char *uri, const char *language,
                     const char *utf8, int64_t version, GError **error);
gboolean ed_lsp_change(EdLsp *, const char *uri, size_t start, size_t end,
                       const char *replacement, int64_t new_version, GError **error);
gboolean ed_lsp_save(EdLsp *, const char *uri, GError **error);
gboolean ed_lsp_close(EdLsp *, const char *uri, GError **error);
const char *ed_lsp_document(EdLsp *, const char *uri, int64_t *version);
uint64_t ed_lsp_request(EdLsp *, const EdLspRequest *, GError **error);
void ed_lsp_cancel(EdLsp *, uint64_t id);
gboolean ed_lsp_reply_apply_edit(EdLsp *, uint64_t token, gboolean applied,
                                const char *failure_reason, GError **error);
/* These pure helpers reject invalid UTF-8, split codepoints/surrogate pairs,
 * out-of-range positions, and offsets in the middle of CRLF. */
gboolean ed_lsp_utf8_position(const char *utf8, size_t offset, EdLspPosition *out);
gboolean ed_lsp_position_utf8(const char *utf8, EdLspPosition pos, size_t *out);
/* Pure validation/application for a single document. Reject overlaps, invalid
 * ranges/UTF-8 and stale version. Caller owns returned text; no disk writes.
 * Equal-position insertions retain server order, per the LSP specification. */
char *ed_lsp_apply_text_edits(const char *utf8, int64_t current_version,
                             const EdLspDocumentEdit *edit, GError **error);
#endif
