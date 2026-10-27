/* Persistent editor sessions and crash recovery. Files are always accessed
 * as EDIT_USER through edit-file.c, never as the screen owner's root. */
#ifndef LOCACHY_EDIT_SESSION_H
#define LOCACHY_EDIT_SESSION_H
#include <stddef.h>
#include "edit-file.h"

struct edit_session_document {
    char *path, *text;
    size_t length;
    struct file_format format;
    int modified, readonly;
    /* Modified file only: git-blob SHA-1 hex of the disk bytes the edits
     * started from, "" when unknown/new. Restore compares it to the disk. */
    char base[2 * DIGEST_SIZE + 1];
};
struct edit_session_view {
    int document;
    size_t cursor, anchor, top;
    int left, sub, wrap, numbers, font_px; /* 0 or 12..96 content pixels */
};
struct edit_session_space {
    struct edit_session_view *tabs;
    size_t ntabs;
    int current;
};
struct edit_session_state {
    struct edit_session_document *documents;
    size_t ndocuments;
    struct edit_session_space *spaces;
    size_t nspaces;
    int active, sidebar, sidebar_width;
    char *layout;                   /* Recursive splitter encoding. */
    int recovered;                  /* Previous checkpoint was not clean. */
};
/* Resource bounds apply before writing and while decoding untrusted state. */
#define EDIT_SESSION_MAX_DOCUMENTS 1024u
#define EDIT_SESSION_MAX_SPACES 64u
#define EDIT_SESSION_MAX_TABS 4096u
#define EDIT_SESSION_MAX_BUFFER (64u * 1024u * 1024u)
#define EDIT_SESSION_MAX_TEXT (128u * 1024u * 1024u)
#define EDIT_SESSION_MAX_FILE (192u * 1024u * 1024u)
#define EDIT_SESSION_MAX_NAME 128u

/* All operations return 0 or an errno. NAME is a UTF-8 basename (1..128
 * bytes, no leading dot, slash, backslash or control characters), not a path;
 * NULL means "default". HOME is honored when running as uid 1000; a root
 * caller uses getpwuid(1000), never root's HOME. Sessions live below
 * ~/.local/state/locachy/edit/sessions, with private directories and files.
 *
 * Layout grammar matches edit_layout_encode/decode: leaf <space-index>;
 * branch V(<ratio>,<node>,<node>) or H(<ratio>,<node>,<node>). V means
 * left/right, H top/bottom, ratio is 1..9999 ten-thousandths. There must be
 * exactly one leaf per space. No whitespace or leading zeroes.
 *
 * Buffers are checkpointed only for modified/untitled documents. Their text
 * is byte-counted (embedded NUL is preserved), including on a clean exit.
 * Loaded unmodified file documents have text == NULL and the saved length;
 * the caller reloads their disk contents and clamps stale view positions.
 * Path NULL (or empty when saving) means an untitled document. Empty welcome
 * sessions have zero documents and spaces with ntabs=0/current=-1. Typed view
 * font_px is 0 (system size) or 12..96 independent content pixels.
 * Load replaces *state only on success; initialize it to zero before first
 * use. Missing sessions return ENOENT and never create directories. Failed
 * loads/writes retain the old checkpoint. Saving over malformed existing
 * state is refused; explicit remove is required to discard it. */
int edit_session_save(const char *name, const struct edit_session_state *state, int clean);
/* As save, but refuse any existing name atomically (EEXIST). */
int edit_session_create(const char *name, const struct edit_session_state *state, int clean);
int edit_session_load(const char *name, struct edit_session_state *state);
void edit_session_free(struct edit_session_state *state);
int edit_session_remove(const char *name);
/* The last explicitly selected session. ENOENT means no selection yet. */
int edit_session_selected(char **name);
int edit_session_select(const char *name);
int edit_session_list(char ***names, size_t *count);
void edit_session_list_free(char **names, size_t count);
#endif
