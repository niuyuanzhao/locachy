/*
 * search-index.h: portable SQLite/FTS5 index engine for LOCachy global search.
 *
 * Layer 1 (names): external-content FTS5 trigram index over files.name for
 * queries of 3+ Unicode characters, plus a NOCASE index range / bounded LIKE
 * scan for 1-2 character queries.
 *
 * Layer 2 (content): contentless_delete=1 FTS5 trigram index (docs_fts) for
 * queries of 3+ Unicode characters, paired with a unicode61 FTS5 table
 * (docs_uni) for 1-2 character prefix queries and snippet extraction.
 *
 * Portable C + POSIX (faccessat) + sqlite3 (>= 3.43; 3.45+ recommended).
 * No Linux-only headers.
 */
#ifndef LOCACHY_SEARCH_INDEX_H
#define LOCACHY_SEARCH_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define SEARCH_SCHEMA_VERSION 1
#define SEARCH_MAX_RESULTS    200
#define SEARCH_QUERY_MAX      256
#define SEARCH_PATH_MAX       4096
#define SEARCH_NAME_MAX       256
#define SEARCH_SNIPPET_MAX    192

/* Return codes (negative on error). */
#define SEARCH_OK             0
#define SEARCH_ERR           -1
#define SEARCH_ERR_SCHEMA    -2 /* incompatible schema version or foreign DB */
#define SEARCH_ERR_SQLITE    -3 /* SQLite library too old or SQLite error */
#define SEARCH_ERR_INVAL     -4 /* invalid argument or query */
#define SEARCH_ERR_NOTFOUND  -5 /* row does not exist */

/* Entry kinds stored in files.kind and reported in search results. */
enum search_kind {
    SEARCH_KIND_FILE    = 0,
    SEARCH_KIND_TEXT    = 1,
    SEARCH_KIND_DOC     = 2,
    SEARCH_KIND_IMAGE   = 3,
    SEARCH_KIND_MEDIA   = 4,
    SEARCH_KIND_ARCHIVE = 5,
    SEARCH_KIND_DIR     = 6
};

/* Document extraction states stored in files.text_state. */
enum search_text_state {
    SEARCH_TEXT_PENDING = 0, /* eligible, not yet extracted */
    SEARCH_TEXT_INDEXED = 1, /* body stored in docs_fts and docs_uni */
    SEARCH_TEXT_SKIPPED = 2, /* binary, too large, unsupported, or failed */
    SEARCH_TEXT_STALE   = 3  /* previously indexed; file changed on disk */
};

struct search_index;

struct search_hit {
    int64_t file_id;
    int64_t dir_id;
    int64_t size;
    int64_t mtime;
    int     layer; /* 1 = names, 2 = content */
    int     kind;  /* enum search_kind */
    double  score; /* higher is better */
    char    path[SEARCH_PATH_MAX];
    char    name[SEARCH_NAME_MAX];
    char    snippet[SEARCH_SNIPPET_MAX];
};

struct search_pending_doc {
    int64_t file_id;
    int64_t size;
    int64_t mtime;
    char    path[SEARCH_PATH_MAX];
};

/* Open or create the search index database at db_path.
 * Sets auto_vacuum=INCREMENTAL before table creation on a fresh DB, configures
 * WAL/NORMAL/MEMORY pragmas, and verifies PRAGMA user_version against
 * SEARCH_SCHEMA_VERSION. Returns SEARCH_OK on success. */
int  search_index_open(const char *db_path, struct search_index **out);
void search_index_close(struct search_index *idx);

/* Last diagnostic error string (empty if none). */
const char *search_index_errmsg(const struct search_index *idx);

/* Static protocol name for enum search_kind ("file", "text", "doc", ...). */
const char *search_kind_name(int kind);

/* Explicit transaction batching. */
int  search_index_begin(struct search_index *idx);
int  search_index_commit(struct search_index *idx);
int  search_index_rollback(struct search_index *idx);
int  search_index_in_tx(const struct search_index *idx);

/* Crawl generation bookkeeping and offline-deletion sweep. */
int64_t search_index_current_gen(const struct search_index *idx);
int64_t search_index_next_gen(struct search_index *idx);
int     search_index_sweep_gen(struct search_index *idx, int64_t min_gen,
                               int64_t *deleted_files, int64_t *deleted_dirs);

/* Directory operations. Use parent_id = 0 for crawl roots (full root path as
 * name); use parent_id > 0 with the single component basename for subdirs.
 * Returns directory id (> 0) or a negative SEARCH_ERR_* code. */
int64_t search_index_upsert_dir(struct search_index *idx, int64_t parent_id,
                                const char *name, int64_t mtime, int64_t gen);
int64_t search_index_find_dir(struct search_index *idx, int64_t parent_id,
                              const char *name, int64_t *mtime_out);
/* Move/rename a directory with a single UPDATE on dirs. */
int     search_index_move_dir(struct search_index *idx, int64_t dir_id,
                              int64_t new_parent_id, const char *new_name);
/* Delete dir_id and its entire descendant subtree (dirs, files, FTS rows)
 * with one recursive query. */
int     search_index_delete_subtree(struct search_index *idx, int64_t dir_id);
/* Build full filesystem path for dir_id (using an internal hot-path cache). */
int     search_index_dir_path(struct search_index *idx, int64_t dir_id,
                              char *out, size_t out_size);

/* Single-directory reconciliation helpers for unwatched-directory mtime sweeps:
 * mark direct children stale (gen = -1), upsert surviving entries with current
 * gen, then prune any remaining stale children (and subtrees of gone subdirs). */
int     search_index_mark_dir_children_stale(struct search_index *idx, int64_t dir_id);
int     search_index_prune_dir_stale(struct search_index *idx, int64_t dir_id);

/* File operations. Returns file id (> 0) or negative SEARCH_ERR_* code.
 * Unchanged (mtime, size) preserves existing text_state; modified files
 * transition INDEXED/STALE -> STALE and SKIPPED/PENDING -> initial_text_state. */
int64_t search_index_upsert_file(struct search_index *idx, int64_t dir_id,
                                 const char *name, int64_t size, int64_t mtime,
                                 int kind, int64_t gen, int initial_text_state);
int64_t search_index_find_file(struct search_index *idx, int64_t dir_id,
                               const char *name);
/* Move/rename a file with a single UPDATE on files (trigger updates names_fts). */
int     search_index_move_file(struct search_index *idx, int64_t file_id,
                               int64_t new_dir_id, const char *new_name);
int     search_index_delete_file(struct search_index *idx, int64_t file_id);
int     search_index_delete_file_by_name(struct search_index *idx, int64_t dir_id,
                                         const char *name);

/* Layer 2 document body management. */
int     search_index_set_text(struct search_index *idx, int64_t file_id,
                              const char *text, size_t text_len);
int     search_index_clear_text(struct search_index *idx, int64_t file_id,
                                int new_text_state);
int     search_index_next_pending_docs(struct search_index *idx,
                                       struct search_pending_doc *out,
                                       size_t cap, size_t *count_out);

/* Ranked queries (capped at SEARCH_MAX_RESULTS = 200).
 * User input is validated and passed exclusively via bound parameters. */
int     search_index_query_names(struct search_index *idx, const char *query,
                                 size_t limit, struct search_hit *out,
                                 size_t *count_out);
int     search_index_query_content(struct search_index *idx, const char *query,
                                   size_t limit, struct search_hit *out,
                                   size_t *count_out);

/* Ghost filter: runs faccessat(AT_FDCWD, path, F_OK, 0) on the first top_n
 * results (0 checks all). Drops entries missing on disk (ENOENT/ENOTDIR) in
 * place while preserving order, and records up to gone_cap dropped file IDs
 * for deferred removal. Returns total dropped count. */
size_t  search_index_filter_ghosts(struct search_hit *hits, size_t *count,
                                   size_t top_n,
                                   int64_t *gone_ids, size_t gone_cap,
                                   size_t *gone_count);

/* Reclaim up to pages freelist pages via PRAGMA incremental_vacuum. */
int     search_index_incremental_vacuum(struct search_index *idx, int pages);

#endif
