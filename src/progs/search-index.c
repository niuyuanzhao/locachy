/*
 * search-index.c: portable SQLite/FTS5 index engine for LOCachy global search.
 *
 * Strictly standard C + POSIX (faccessat) + sqlite3; no Linux-only headers.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "search-index.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_CACHE_SLOTS 1024 /* power of two */
#define PATH_CACHE_MASK  (PATH_CACHE_SLOTS - 1)
#define DIR_MAX_DEPTH    256
#define SHORT_LIKE_SCAN  2000

struct path_cache_entry {
    int64_t  dir_id;
    uint64_t stamp;
    char     path[SEARCH_PATH_MAX];
};

struct search_index {
    sqlite3 *db;
    int      in_tx;
    int64_t  current_gen;
    uint64_t path_cache_epoch;
    struct path_cache_entry path_cache[PATH_CACHE_SLOTS];

    /* Prepared statements. */
    sqlite3_stmt *st_dir_upsert;
    sqlite3_stmt *st_dir_find;
    sqlite3_stmt *st_dir_parent;
    sqlite3_stmt *st_dir_move;
    sqlite3_stmt *st_file_find;
    sqlite3_stmt *st_file_insert;
    sqlite3_stmt *st_file_update;
    sqlite3_stmt *st_file_move;
    sqlite3_stmt *st_file_delete;
    sqlite3_stmt *st_file_delete_name;
    sqlite3_stmt *st_doc_fts_ins;
    sqlite3_stmt *st_doc_fts_del;
    sqlite3_stmt *st_doc_uni_ins;
    sqlite3_stmt *st_doc_uni_del;
    sqlite3_stmt *st_file_text_state;
    sqlite3_stmt *st_pending_docs;
    sqlite3_stmt *st_query_names_fts;
    sqlite3_stmt *st_query_names_prefix;
    sqlite3_stmt *st_query_names_like;
    sqlite3_stmt *st_query_docs_fts;
    sqlite3_stmt *st_query_docs_uni;
    sqlite3_stmt *st_meta_set;
    sqlite3_stmt *st_meta_get;

    char errmsg[256];
};

static void set_err(struct search_index *idx, const char *fmt, ...)
{
    if (!idx) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(idx->errmsg, sizeof idx->errmsg, fmt, ap);
    va_end(ap);
}

static void set_sql_err(struct search_index *idx, const char *ctx)
{
    if (!idx) return;
    const char *m = idx->db ? sqlite3_errmsg(idx->db) : "no database";
    set_err(idx, "%s: %s", ctx, m ? m : "unknown sqlite error");
}

const char *search_index_errmsg(const struct search_index *idx)
{
    return idx ? idx->errmsg : "";
}

const char *search_kind_name(int kind)
{
    switch (kind) {
    case SEARCH_KIND_FILE:    return "file";
    case SEARCH_KIND_TEXT:    return "text";
    case SEARCH_KIND_DOC:     return "doc";
    case SEARCH_KIND_IMAGE:   return "image";
    case SEARCH_KIND_MEDIA:   return "media";
    case SEARCH_KIND_ARCHIVE: return "archive";
    case SEARCH_KIND_DIR:     return "dir";
    default:                  return "file";
    }
}

static void invalidate_path_cache(struct search_index *idx)
{
    if (!idx) return;
    idx->path_cache_epoch++;
    if (idx->path_cache_epoch == 0) {
        memset(idx->path_cache, 0, sizeof idx->path_cache);
        idx->path_cache_epoch = 1;
    }
}

static size_t path_cache_slot(int64_t dir_id)
{
    uint64_t h = (uint64_t)dir_id * 11400714819323198485ULL;
    return (size_t)((h >> 54) & PATH_CACHE_MASK);
}

static int exec_sql(struct search_index *idx, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(idx->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        set_err(idx, "sqlite exec failed (%d): %s", rc, err ? err : sqlite3_errmsg(idx->db));
        sqlite3_free(err);
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

static int query_int64_pragma(struct search_index *idx, const char *sql, int64_t *out)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(idx->db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        set_sql_err(idx, sql);
        return SEARCH_ERR_SQLITE;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return SEARCH_OK;
    }
    sqlite3_finalize(st);
    set_sql_err(idx, sql);
    return SEARCH_ERR_SQLITE;
}

static int prepare_stmt(struct search_index *idx, const char *sql, sqlite3_stmt **out)
{
    int rc = sqlite3_prepare_v2(idx->db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) {
        set_sql_err(idx, "prepare");
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

static void finalize_all(struct search_index *idx)
{
    sqlite3_stmt **stmts[] = {
        &idx->st_dir_upsert,
        &idx->st_dir_find,
        &idx->st_dir_parent,
        &idx->st_dir_move,
        &idx->st_file_find,
        &idx->st_file_insert,
        &idx->st_file_update,
        &idx->st_file_move,
        &idx->st_file_delete,
        &idx->st_file_delete_name,
        &idx->st_doc_fts_ins,
        &idx->st_doc_fts_del,
        &idx->st_doc_uni_ins,
        &idx->st_doc_uni_del,
        &idx->st_file_text_state,
        &idx->st_pending_docs,
        &idx->st_query_names_fts,
        &idx->st_query_names_prefix,
        &idx->st_query_names_like,
        &idx->st_query_docs_fts,
        &idx->st_query_docs_uni,
        &idx->st_meta_set,
        &idx->st_meta_get,
    };
    for (size_t i = 0; i < sizeof stmts / sizeof stmts[0]; i++) {
        if (*stmts[i]) {
            sqlite3_finalize(*stmts[i]);
            *stmts[i] = NULL;
        }
    }
}

static int create_schema(struct search_index *idx)
{
    static const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS meta ("
        "    key TEXT PRIMARY KEY,"
        "    value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS dirs ("
        "    id INTEGER PRIMARY KEY,"
        "    parent INTEGER NOT NULL DEFAULT 0,"
        "    name TEXT NOT NULL,"
        "    mtime INTEGER NOT NULL DEFAULT 0,"
        "    gen INTEGER NOT NULL,"
        "    UNIQUE(parent, name)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_dirs_parent ON dirs(parent);"
        "CREATE INDEX IF NOT EXISTS idx_dirs_gen ON dirs(gen);"
        "CREATE TABLE IF NOT EXISTS files ("
        "    id INTEGER PRIMARY KEY,"
        "    dir INTEGER NOT NULL,"
        "    name TEXT NOT NULL,"
        "    size INTEGER NOT NULL DEFAULT 0,"
        "    mtime INTEGER NOT NULL DEFAULT 0,"
        "    kind INTEGER NOT NULL DEFAULT 0,"
        "    gen INTEGER NOT NULL,"
        "    text_state INTEGER NOT NULL DEFAULT 0,"
        "    UNIQUE(dir, name)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_files_dir_name ON files(dir, name);"
        "CREATE INDEX IF NOT EXISTS idx_files_gen ON files(gen);"
        "CREATE INDEX IF NOT EXISTS idx_files_mtime ON files(mtime DESC);"
        "CREATE INDEX IF NOT EXISTS idx_files_name_nocase ON files(name COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS idx_files_text_state ON files(text_state) "
        "    WHERE text_state IN (0, 3);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS names_fts USING fts5("
        "    name,"
        "    content='files',"
        "    content_rowid='id',"
        "    tokenize='trigram'"
        ");"
        "CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN "
        "    INSERT INTO names_fts(rowid, name) VALUES (new.id, new.name);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN "
        "    INSERT INTO names_fts(names_fts, rowid, name) VALUES ('delete', old.id, old.name);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE OF name ON files "
        "WHEN old.name IS NOT new.name BEGIN "
        "    INSERT INTO names_fts(names_fts, rowid, name) VALUES ('delete', old.id, old.name);"
        "    INSERT INTO names_fts(rowid, name) VALUES (new.id, new.name);"
        "END;"
        "CREATE VIRTUAL TABLE IF NOT EXISTS docs_fts USING fts5("
        "    body,"
        "    content='',"
        "    contentless_delete=1,"
        "    tokenize='trigram'"
        ");"
        "CREATE VIRTUAL TABLE IF NOT EXISTS docs_uni USING fts5("
        "    body,"
        "    tokenize='unicode61'"
        ");"
        "INSERT OR IGNORE INTO meta(key, value) VALUES ('generation', '0');";

    if (exec_sql(idx, "BEGIN IMMEDIATE;") != SEARCH_OK) return SEARCH_ERR_SQLITE;
    if (exec_sql(idx, schema_sql) != SEARCH_OK) {
        (void)exec_sql(idx, "ROLLBACK;");
        return SEARCH_ERR_SQLITE;
    }
    char ver_sql[64];
    snprintf(ver_sql, sizeof ver_sql, "PRAGMA user_version=%d;", SEARCH_SCHEMA_VERSION);
    if (exec_sql(idx, ver_sql) != SEARCH_OK) {
        (void)exec_sql(idx, "ROLLBACK;");
        return SEARCH_ERR_SQLITE;
    }
    if (exec_sql(idx, "COMMIT;") != SEARCH_OK) {
        (void)exec_sql(idx, "ROLLBACK;");
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

static int prepare_all(struct search_index *idx)
{
    if (prepare_stmt(idx,
        "INSERT INTO dirs(parent, name, mtime, gen) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(parent, name) DO UPDATE SET mtime=excluded.mtime, gen=excluded.gen "
        "RETURNING id;",
        &idx->st_dir_upsert) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT id, mtime FROM dirs WHERE parent=?1 AND name=?2;",
        &idx->st_dir_find) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT parent, name FROM dirs WHERE id=?1;",
        &idx->st_dir_parent) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "UPDATE dirs SET parent=?2, name=?3 WHERE id=?1;",
        &idx->st_dir_move) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT id, size, mtime, text_state FROM files WHERE dir=?1 AND name=?2;",
        &idx->st_file_find) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "INSERT INTO files(dir, name, size, mtime, kind, gen, text_state) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) RETURNING id;",
        &idx->st_file_insert) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "UPDATE files SET size=?2, mtime=?3, kind=?4, gen=?5, text_state=?6 "
        "WHERE id=?1;",
        &idx->st_file_update) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "UPDATE files SET dir=?2, name=?3 WHERE id=?1;",
        &idx->st_file_move) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "DELETE FROM files WHERE id=?1;",
        &idx->st_file_delete) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "DELETE FROM files WHERE dir=?1 AND name=?2 RETURNING id;",
        &idx->st_file_delete_name) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "INSERT INTO docs_fts(rowid, body) VALUES (?1, ?2);",
        &idx->st_doc_fts_ins) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "DELETE FROM docs_fts WHERE rowid=?1;",
        &idx->st_doc_fts_del) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "INSERT INTO docs_uni(rowid, body) VALUES (?1, ?2);",
        &idx->st_doc_uni_ins) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "DELETE FROM docs_uni WHERE rowid=?1;",
        &idx->st_doc_uni_del) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "UPDATE files SET text_state=?2 WHERE id=?1;",
        &idx->st_file_text_state) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT id, dir, name, size, mtime FROM files "
        "WHERE text_state IN (0, 3) ORDER BY mtime DESC, id ASC LIMIT ?1;",
        &idx->st_pending_docs) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT f.id, f.dir, f.name, f.size, f.mtime, f.kind, bm25(names_fts) AS rank "
        "FROM names_fts JOIN files f ON f.id = names_fts.rowid "
        "WHERE names_fts MATCH ?1 "
        "ORDER BY rank ASC, length(f.name) ASC, f.mtime DESC LIMIT ?2;",
        &idx->st_query_names_fts) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Prefix index range scan on idx_files_name_nocase (used for 1-2 char queries). */
    if (prepare_stmt(idx,
        "SELECT id, dir, name, size, mtime, kind "
        "FROM files "
        "WHERE name COLLATE NOCASE >= ?1 AND name COLLATE NOCASE < ?2 "
        "ORDER BY length(name) ASC, mtime DESC LIMIT ?3;",
        &idx->st_query_names_prefix) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Bounded LIKE scan for 1-2 char substring matches (non-prefix fallback). */
    if (prepare_stmt(idx,
        "SELECT id, dir, name, size, mtime, kind FROM ("
        "    SELECT id, dir, name, size, mtime, kind FROM files "
        "    ORDER BY mtime DESC LIMIT ?3"
        ") WHERE name LIKE ?1 ESCAPE '\\' "
        "ORDER BY length(name) ASC, mtime DESC LIMIT ?2;",
        &idx->st_query_names_like) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Layer 2 (3+ chars): match via docs_fts (trigram) with LIMIT first, then
     * look up docs_uni.body only for the top rows (docs_fts is contentless). */
    if (prepare_stmt(idx,
        "SELECT m.id, m.dir, m.name, m.size, m.mtime, m.kind, m.rank, "
        "       COALESCE(substr(u.body, 1, 16384), '') "
        "FROM ("
        "    SELECT f.id, f.dir, f.name, f.size, f.mtime, f.kind, bm25(docs_fts) AS rank "
        "    FROM docs_fts JOIN files f ON f.id = docs_fts.rowid "
        "    WHERE docs_fts MATCH ?1 "
        "    ORDER BY rank ASC, f.mtime DESC LIMIT ?2"
        ") m "
        "LEFT JOIN docs_uni u ON u.rowid = m.id "
        "ORDER BY m.rank ASC, m.mtime DESC;",
        &idx->st_query_docs_fts) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Layer 2 (1-2 chars): unicode61 prefix match (q*), with FTS5 snippet. */
    if (prepare_stmt(idx,
        "SELECT f.id, f.dir, f.name, f.size, f.mtime, f.kind, bm25(docs_uni) AS rank, "
        "       snippet(docs_uni, 0, '', '', '...', 16) "
        "FROM docs_uni JOIN files f ON f.id = docs_uni.rowid "
        "WHERE docs_uni MATCH ?1 "
        "ORDER BY rank ASC, f.mtime DESC LIMIT ?2;",
        &idx->st_query_docs_uni) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "INSERT INTO meta(key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        &idx->st_meta_set) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (prepare_stmt(idx,
        "SELECT value FROM meta WHERE key=?1;",
        &idx->st_meta_get) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    return SEARCH_OK;
}

static int load_current_gen(struct search_index *idx)
{
    sqlite3_reset(idx->st_meta_get);
    sqlite3_clear_bindings(idx->st_meta_get);
    sqlite3_bind_text(idx->st_meta_get, 1, "generation", -1, SQLITE_STATIC);
    int rc = sqlite3_step(idx->st_meta_get);
    if (rc == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(idx->st_meta_get, 0);
        idx->current_gen = val ? strtoll(val, NULL, 10) : 0;
    } else if (rc == SQLITE_DONE) {
        idx->current_gen = 0;
    } else {
        set_sql_err(idx, "load generation");
        sqlite3_reset(idx->st_meta_get);
        return SEARCH_ERR_SQLITE;
    }
    sqlite3_reset(idx->st_meta_get);
    return SEARCH_OK;
}

int search_index_open(const char *db_path, struct search_index **out)
{
    if (!db_path || !db_path[0] || !out) return SEARCH_ERR_INVAL;
    *out = NULL;

    if (sqlite3_libversion_number() < 3043000) {
        return SEARCH_ERR_SQLITE;
    }

    struct search_index *idx = calloc(1, sizeof *idx);
    if (!idx) return SEARCH_ERR;
    idx->path_cache_epoch = 1;

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(db_path, &idx->db, flags, NULL);
    if (rc != SQLITE_OK) {
        search_index_close(idx);
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_busy_timeout(idx->db, 2000);

    /* Inspect whether the DB is brand new before touching page-header pragmas. */
    int64_t page_count = 0;
    int64_t user_ver = 0;
    if (query_int64_pragma(idx, "PRAGMA page_count;", &page_count) != SEARCH_OK ||
        query_int64_pragma(idx, "PRAGMA user_version;", &user_ver) != SEARCH_OK) {
        search_index_close(idx);
        return SEARCH_ERR_SQLITE;
    }

    /* auto_vacuum=INCREMENTAL MUST be set before the first table is created. */
    if (page_count == 0) {
        if (exec_sql(idx, "PRAGMA auto_vacuum=INCREMENTAL;") != SEARCH_OK) {
            search_index_close(idx);
            return SEARCH_ERR_SQLITE;
        }
    }

    if (exec_sql(idx,
                 "PRAGMA journal_mode=WAL;"
                 "PRAGMA synchronous=NORMAL;"
                 "PRAGMA temp_store=MEMORY;"
                 "PRAGMA cache_size=-4000;") != SEARCH_OK) {
        search_index_close(idx);
        return SEARCH_ERR_SQLITE;
    }

    if (user_ver == 0) {
        if (page_count > 0) {
            /* Check if non-empty database already contains unrelated tables. */
            int64_t table_count = 0;
            if (query_int64_pragma(idx,
                    "SELECT count(*) FROM sqlite_master WHERE type='table';",
                    &table_count) != SEARCH_OK) {
                search_index_close(idx);
                return SEARCH_ERR_SQLITE;
            }
            if (table_count > 0) {
                search_index_close(idx);
                return SEARCH_ERR_SCHEMA;
            }
        }
        if (create_schema(idx) != SEARCH_OK) {
            search_index_close(idx);
            return SEARCH_ERR_SQLITE;
        }
    } else if (user_ver != SEARCH_SCHEMA_VERSION) {
        search_index_close(idx);
        return SEARCH_ERR_SCHEMA;
    }

    if (prepare_all(idx) != SEARCH_OK || load_current_gen(idx) != SEARCH_OK) {
        search_index_close(idx);
        return SEARCH_ERR_SQLITE;
    }

    *out = idx;
    return SEARCH_OK;
}

void search_index_close(struct search_index *idx)
{
    if (!idx) return;
    if (idx->in_tx && idx->db) {
        (void)exec_sql(idx, "ROLLBACK;");
        idx->in_tx = 0;
    }
    finalize_all(idx);
    if (idx->db) {
        sqlite3_close(idx->db);
        idx->db = NULL;
    }
    free(idx);
}

int search_index_begin(struct search_index *idx)
{
    if (!idx || !idx->db) return SEARCH_ERR_INVAL;
    if (idx->in_tx) return SEARCH_OK;
    if (exec_sql(idx, "BEGIN IMMEDIATE;") != SEARCH_OK) return SEARCH_ERR_SQLITE;
    idx->in_tx = 1;
    return SEARCH_OK;
}

int search_index_commit(struct search_index *idx)
{
    if (!idx || !idx->db) return SEARCH_ERR_INVAL;
    if (!idx->in_tx) return SEARCH_OK;
    if (exec_sql(idx, "COMMIT;") != SEARCH_OK) return SEARCH_ERR_SQLITE;
    idx->in_tx = 0;
    return SEARCH_OK;
}

int search_index_rollback(struct search_index *idx)
{
    if (!idx || !idx->db) return SEARCH_ERR_INVAL;
    if (!idx->in_tx) return SEARCH_OK;
    int rc = exec_sql(idx, "ROLLBACK;");
    idx->in_tx = 0;
    invalidate_path_cache(idx);
    return rc;
}

int search_index_in_tx(const struct search_index *idx)
{
    return idx ? idx->in_tx : 0;
}

int64_t search_index_current_gen(const struct search_index *idx)
{
    return idx ? idx->current_gen : 0;
}

int64_t search_index_next_gen(struct search_index *idx)
{
    if (!idx || !idx->db) return SEARCH_ERR_INVAL;
    int64_t next = idx->current_gen + 1;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)next);

    sqlite3_reset(idx->st_meta_set);
    sqlite3_clear_bindings(idx->st_meta_set);
    sqlite3_bind_text(idx->st_meta_set, 1, "generation", -1, SQLITE_STATIC);
    sqlite3_bind_text(idx->st_meta_set, 2, buf, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(idx->st_meta_set);
    sqlite3_reset(idx->st_meta_set);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "next_gen");
        return SEARCH_ERR_SQLITE;
    }
    idx->current_gen = next;
    return next;
}

static int delete_doc_rows(struct search_index *idx, int64_t file_id)
{
    sqlite3_reset(idx->st_doc_fts_del);
    sqlite3_clear_bindings(idx->st_doc_fts_del);
    sqlite3_bind_int64(idx->st_doc_fts_del, 1, file_id);
    int rc = sqlite3_step(idx->st_doc_fts_del);
    sqlite3_reset(idx->st_doc_fts_del);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "delete docs_fts");
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_reset(idx->st_doc_uni_del);
    sqlite3_clear_bindings(idx->st_doc_uni_del);
    sqlite3_bind_int64(idx->st_doc_uni_del, 1, file_id);
    rc = sqlite3_step(idx->st_doc_uni_del);
    sqlite3_reset(idx->st_doc_uni_del);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "delete docs_uni");
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

int64_t search_index_upsert_dir(struct search_index *idx, int64_t parent_id,
                                const char *name, int64_t mtime, int64_t gen)
{
    if (!idx || !idx->db || parent_id < 0 || !name || !name[0]) return SEARCH_ERR_INVAL;

    sqlite3_reset(idx->st_dir_upsert);
    sqlite3_clear_bindings(idx->st_dir_upsert);
    sqlite3_bind_int64(idx->st_dir_upsert, 1, parent_id);
    sqlite3_bind_text(idx->st_dir_upsert, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(idx->st_dir_upsert, 3, mtime);
    sqlite3_bind_int64(idx->st_dir_upsert, 4, gen);

    int rc = sqlite3_step(idx->st_dir_upsert);
    if (rc != SQLITE_ROW) {
        set_sql_err(idx, "upsert_dir");
        sqlite3_reset(idx->st_dir_upsert);
        return SEARCH_ERR_SQLITE;
    }
    int64_t id = sqlite3_column_int64(idx->st_dir_upsert, 0);
    sqlite3_reset(idx->st_dir_upsert);
    return id;
}

int64_t search_index_find_dir(struct search_index *idx, int64_t parent_id,
                              const char *name, int64_t *mtime_out)
{
    if (!idx || !idx->db || parent_id < 0 || !name || !name[0]) return SEARCH_ERR_INVAL;

    sqlite3_reset(idx->st_dir_find);
    sqlite3_clear_bindings(idx->st_dir_find);
    sqlite3_bind_int64(idx->st_dir_find, 1, parent_id);
    sqlite3_bind_text(idx->st_dir_find, 2, name, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(idx->st_dir_find);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(idx->st_dir_find, 0);
        if (mtime_out) *mtime_out = sqlite3_column_int64(idx->st_dir_find, 1);
        sqlite3_reset(idx->st_dir_find);
        return id;
    }
    sqlite3_reset(idx->st_dir_find);
    if (rc == SQLITE_DONE) return SEARCH_ERR_NOTFOUND;
    set_sql_err(idx, "find_dir");
    return SEARCH_ERR_SQLITE;
}

static int dir_is_descendant(struct search_index *idx, int64_t candidate, int64_t ancestor)
{
    int64_t cur = candidate;
    int depth = 0;
    while (cur > 0 && depth < DIR_MAX_DEPTH) {
        if (cur == ancestor) return 1;
        sqlite3_reset(idx->st_dir_parent);
        sqlite3_clear_bindings(idx->st_dir_parent);
        sqlite3_bind_int64(idx->st_dir_parent, 1, cur);
        int rc = sqlite3_step(idx->st_dir_parent);
        if (rc != SQLITE_ROW) {
            sqlite3_reset(idx->st_dir_parent);
            return 0;
        }
        int64_t parent = sqlite3_column_int64(idx->st_dir_parent, 0);
        sqlite3_reset(idx->st_dir_parent);
        if (parent == cur) return 0;
        cur = parent;
        depth++;
    }
    return 0;
}

int search_index_move_dir(struct search_index *idx, int64_t dir_id,
                          int64_t new_parent_id, const char *new_name)
{
    if (!idx || !idx->db || dir_id <= 0 || new_parent_id < 0 || !new_name || !new_name[0]) {
        return SEARCH_ERR_INVAL;
    }
    if (dir_id == new_parent_id || dir_is_descendant(idx, new_parent_id, dir_id)) {
        return SEARCH_ERR_INVAL;
    }

    /* Remove any stale directory already occupying (new_parent_id, new_name). */
    int64_t existing = search_index_find_dir(idx, new_parent_id, new_name, NULL);
    if (existing > 0 && existing != dir_id) {
        int drc = search_index_delete_subtree(idx, existing);
        if (drc != SEARCH_OK) return drc;
    } else if (existing < 0 && existing != SEARCH_ERR_NOTFOUND) {
        return (int)existing;
    }

    sqlite3_reset(idx->st_dir_move);
    sqlite3_clear_bindings(idx->st_dir_move);
    sqlite3_bind_int64(idx->st_dir_move, 1, dir_id);
    sqlite3_bind_int64(idx->st_dir_move, 2, new_parent_id);
    sqlite3_bind_text(idx->st_dir_move, 3, new_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(idx->st_dir_move);
    sqlite3_reset(idx->st_dir_move);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "move_dir");
        return SEARCH_ERR_SQLITE;
    }
    invalidate_path_cache(idx);
    return sqlite3_changes(idx->db) > 0 ? SEARCH_OK : SEARCH_ERR_NOTFOUND;
}

static int run_recursive_subtree_delete(struct search_index *idx, int64_t dir_id)
{
    static const char *const queries[] = {
        "WITH RECURSIVE sub(id) AS ("
        "    SELECT ?1 UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM docs_fts WHERE rowid IN (SELECT id FROM files WHERE dir IN (SELECT id FROM sub));",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT ?1 UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM docs_uni WHERE rowid IN (SELECT id FROM files WHERE dir IN (SELECT id FROM sub));",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT ?1 UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM files WHERE dir IN (SELECT id FROM sub);",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT ?1 UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM dirs WHERE id IN (SELECT id FROM sub);"
    };

    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (prepare_stmt(idx, queries[i], &st) != SEARCH_OK) return SEARCH_ERR_SQLITE;
        sqlite3_bind_int64(st, 1, dir_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_sql_err(idx, "delete_subtree");
            return SEARCH_ERR_SQLITE;
        }
    }
    return SEARCH_OK;
}

int search_index_delete_subtree(struct search_index *idx, int64_t dir_id)
{
    if (!idx || !idx->db || dir_id <= 0) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    int rc = run_recursive_subtree_delete(idx, dir_id);
    if (rc != SEARCH_OK) {
        if (own_tx) (void)search_index_rollback(idx);
        return rc;
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    invalidate_path_cache(idx);
    return SEARCH_OK;
}

int search_index_dir_path(struct search_index *idx, int64_t dir_id,
                          char *out, size_t out_size)
{
    if (!idx || !idx->db || dir_id <= 0 || !out || out_size < 2) return SEARCH_ERR_INVAL;

    size_t slot = path_cache_slot(dir_id);
    struct path_cache_entry *ce = &idx->path_cache[slot];
    if (ce->stamp == idx->path_cache_epoch && ce->dir_id == dir_id) {
        size_t len = strlen(ce->path);
        if (len + 1 > out_size) return SEARCH_ERR;
        memcpy(out, ce->path, len + 1);
        return SEARCH_OK;
    }

    char *parts[DIR_MAX_DEPTH];
    int depth = 0;
    int64_t cur = dir_id;
    int rc_ret = SEARCH_OK;

    while (cur > 0) {
        if (depth >= DIR_MAX_DEPTH) {
            rc_ret = SEARCH_ERR;
            goto cleanup;
        }
        sqlite3_reset(idx->st_dir_parent);
        sqlite3_clear_bindings(idx->st_dir_parent);
        sqlite3_bind_int64(idx->st_dir_parent, 1, cur);
        int rc = sqlite3_step(idx->st_dir_parent);
        if (rc != SQLITE_ROW) {
            sqlite3_reset(idx->st_dir_parent);
            rc_ret = (rc == SQLITE_DONE) ? SEARCH_ERR_NOTFOUND : SEARCH_ERR_SQLITE;
            goto cleanup;
        }
        int64_t parent = sqlite3_column_int64(idx->st_dir_parent, 0);
        const char *name = (const char *)sqlite3_column_text(idx->st_dir_parent, 1);
        if (!name) {
            sqlite3_reset(idx->st_dir_parent);
            rc_ret = SEARCH_ERR;
            goto cleanup;
        }
        parts[depth] = strdup(name);
        if (!parts[depth]) {
            sqlite3_reset(idx->st_dir_parent);
            rc_ret = SEARCH_ERR;
            goto cleanup;
        }
        depth++;
        sqlite3_reset(idx->st_dir_parent);
        if (parent == cur) {
            rc_ret = SEARCH_ERR;
            goto cleanup;
        }
        cur = parent;
    }

    if (depth == 0) {
        rc_ret = SEARCH_ERR_NOTFOUND;
        goto cleanup;
    }

    char assembled[SEARCH_PATH_MAX];
    size_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        const char *p = parts[i];
        size_t plen = strlen(p);
        if (i < depth - 1) {
            if (pos > 0 && assembled[pos - 1] != '/') {
                if (pos + 1 >= sizeof assembled) { rc_ret = SEARCH_ERR; goto cleanup; }
                assembled[pos++] = '/';
            }
        }
        if (pos + plen >= sizeof assembled) { rc_ret = SEARCH_ERR; goto cleanup; }
        memcpy(assembled + pos, p, plen);
        pos += plen;
    }
    assembled[pos] = '\0';

    ce->dir_id = dir_id;
    ce->stamp = idx->path_cache_epoch;
    memcpy(ce->path, assembled, pos + 1);

    if (pos + 1 > out_size) {
        rc_ret = SEARCH_ERR;
        goto cleanup;
    }
    memcpy(out, assembled, pos + 1);

cleanup:
    for (int i = 0; i < depth; i++) free(parts[i]);
    return rc_ret;
}

int search_index_mark_dir_children_stale(struct search_index *idx, int64_t dir_id)
{
    if (!idx || !idx->db || dir_id <= 0) return SEARCH_ERR_INVAL;
    sqlite3_stmt *st = NULL;
    if (prepare_stmt(idx, "UPDATE files SET gen=-1 WHERE dir=?1;", &st) != SEARCH_OK) {
        return SEARCH_ERR_SQLITE;
    }
    sqlite3_bind_int64(st, 1, dir_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "mark_dir_children_stale files");
        return SEARCH_ERR_SQLITE;
    }

    if (prepare_stmt(idx, "UPDATE dirs SET gen=-1 WHERE parent=?1;", &st) != SEARCH_OK) {
        return SEARCH_ERR_SQLITE;
    }
    sqlite3_bind_int64(st, 1, dir_id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "mark_dir_children_stale dirs");
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

int search_index_prune_dir_stale(struct search_index *idx, int64_t dir_id)
{
    if (!idx || !idx->db || dir_id <= 0) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Delete direct files still marked stale. */
    static const char *const file_stmts[] = {
        "DELETE FROM docs_fts WHERE rowid IN (SELECT id FROM files WHERE dir=?1 AND gen=-1);",
        "DELETE FROM docs_uni WHERE rowid IN (SELECT id FROM files WHERE dir=?1 AND gen=-1);",
        "DELETE FROM files WHERE dir=?1 AND gen=-1;"
    };
    for (size_t i = 0; i < sizeof file_stmts / sizeof file_stmts[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (prepare_stmt(idx, file_stmts[i], &st) != SEARCH_OK) {
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
        sqlite3_bind_int64(st, 1, dir_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_sql_err(idx, "prune_dir_stale files");
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
    }

    /* Delete any direct subdirectories still marked stale along with their subtrees. */
    static const char *const dir_stmts[] = {
        "WITH RECURSIVE sub(id) AS ("
        "    SELECT id FROM dirs WHERE parent=?1 AND gen=-1 "
        "    UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM docs_fts WHERE rowid IN (SELECT id FROM files WHERE dir IN (SELECT id FROM sub));",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT id FROM dirs WHERE parent=?1 AND gen=-1 "
        "    UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM docs_uni WHERE rowid IN (SELECT id FROM files WHERE dir IN (SELECT id FROM sub));",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT id FROM dirs WHERE parent=?1 AND gen=-1 "
        "    UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM files WHERE dir IN (SELECT id FROM sub);",

        "WITH RECURSIVE sub(id) AS ("
        "    SELECT id FROM dirs WHERE parent=?1 AND gen=-1 "
        "    UNION ALL SELECT d.id FROM dirs d JOIN sub s ON d.parent = s.id"
        ") DELETE FROM dirs WHERE id IN (SELECT id FROM sub);"
    };
    for (size_t i = 0; i < sizeof dir_stmts / sizeof dir_stmts[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (prepare_stmt(idx, dir_stmts[i], &st) != SEARCH_OK) {
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
        sqlite3_bind_int64(st, 1, dir_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_sql_err(idx, "prune_dir_stale dirs");
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    invalidate_path_cache(idx);
    return SEARCH_OK;
}

int64_t search_index_upsert_file(struct search_index *idx, int64_t dir_id,
                                 const char *name, int64_t size, int64_t mtime,
                                 int kind, int64_t gen, int initial_text_state)
{
    if (!idx || !idx->db || dir_id <= 0 || !name || !name[0]) return SEARCH_ERR_INVAL;

    sqlite3_reset(idx->st_file_find);
    sqlite3_clear_bindings(idx->st_file_find);
    sqlite3_bind_int64(idx->st_file_find, 1, dir_id);
    sqlite3_bind_text(idx->st_file_find, 2, name, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(idx->st_file_find);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(idx->st_file_find, 0);
        int64_t old_size = sqlite3_column_int64(idx->st_file_find, 1);
        int64_t old_mtime = sqlite3_column_int64(idx->st_file_find, 2);
        int old_state = sqlite3_column_int(idx->st_file_find, 3);
        sqlite3_reset(idx->st_file_find);

        int new_state = old_state;
        if (old_size != size || old_mtime != mtime) {
            if (old_state == SEARCH_TEXT_INDEXED || old_state == SEARCH_TEXT_STALE) {
                new_state = (initial_text_state == SEARCH_TEXT_SKIPPED)
                            ? SEARCH_TEXT_SKIPPED
                            : SEARCH_TEXT_STALE;
                if (new_state == SEARCH_TEXT_SKIPPED) {
                    if (delete_doc_rows(idx, id) != SEARCH_OK) return SEARCH_ERR_SQLITE;
                }
            } else {
                new_state = initial_text_state;
            }
        }

        sqlite3_reset(idx->st_file_update);
        sqlite3_clear_bindings(idx->st_file_update);
        sqlite3_bind_int64(idx->st_file_update, 1, id);
        sqlite3_bind_int64(idx->st_file_update, 2, size);
        sqlite3_bind_int64(idx->st_file_update, 3, mtime);
        sqlite3_bind_int(idx->st_file_update, 4, kind);
        sqlite3_bind_int64(idx->st_file_update, 5, gen);
        sqlite3_bind_int(idx->st_file_update, 6, new_state);
        rc = sqlite3_step(idx->st_file_update);
        sqlite3_reset(idx->st_file_update);
        if (rc != SQLITE_DONE) {
            set_sql_err(idx, "update_file");
            return SEARCH_ERR_SQLITE;
        }
        return id;
    }
    sqlite3_reset(idx->st_file_find);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "find_file");
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_reset(idx->st_file_insert);
    sqlite3_clear_bindings(idx->st_file_insert);
    sqlite3_bind_int64(idx->st_file_insert, 1, dir_id);
    sqlite3_bind_text(idx->st_file_insert, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(idx->st_file_insert, 3, size);
    sqlite3_bind_int64(idx->st_file_insert, 4, mtime);
    sqlite3_bind_int(idx->st_file_insert, 5, kind);
    sqlite3_bind_int64(idx->st_file_insert, 6, gen);
    sqlite3_bind_int(idx->st_file_insert, 7, initial_text_state);
    rc = sqlite3_step(idx->st_file_insert);
    if (rc != SQLITE_ROW) {
        set_sql_err(idx, "insert_file");
        sqlite3_reset(idx->st_file_insert);
        return SEARCH_ERR_SQLITE;
    }
    int64_t id = sqlite3_column_int64(idx->st_file_insert, 0);
    sqlite3_reset(idx->st_file_insert);
    return id;
}

int64_t search_index_find_file(struct search_index *idx, int64_t dir_id,
                               const char *name)
{
    if (!idx || !idx->db || dir_id <= 0 || !name || !name[0]) return SEARCH_ERR_INVAL;
    sqlite3_reset(idx->st_file_find);
    sqlite3_clear_bindings(idx->st_file_find);
    sqlite3_bind_int64(idx->st_file_find, 1, dir_id);
    sqlite3_bind_text(idx->st_file_find, 2, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(idx->st_file_find);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(idx->st_file_find, 0);
        sqlite3_reset(idx->st_file_find);
        return id;
    }
    sqlite3_reset(idx->st_file_find);
    if (rc == SQLITE_DONE) return SEARCH_ERR_NOTFOUND;
    set_sql_err(idx, "find_file");
    return SEARCH_ERR_SQLITE;
}

int search_index_move_file(struct search_index *idx, int64_t file_id,
                           int64_t new_dir_id, const char *new_name)
{
    if (!idx || !idx->db || file_id <= 0 || new_dir_id <= 0 || !new_name || !new_name[0]) {
        return SEARCH_ERR_INVAL;
    }

    /* If a different file occupies the destination (rename-over), delete it first. */
    int64_t existing = search_index_find_file(idx, new_dir_id, new_name);
    if (existing > 0 && existing != file_id) {
        int drc = search_index_delete_file(idx, existing);
        if (drc != SEARCH_OK) return drc;
    } else if (existing < 0 && existing != SEARCH_ERR_NOTFOUND) {
        return (int)existing;
    }

    sqlite3_reset(idx->st_file_move);
    sqlite3_clear_bindings(idx->st_file_move);
    sqlite3_bind_int64(idx->st_file_move, 1, file_id);
    sqlite3_bind_int64(idx->st_file_move, 2, new_dir_id);
    sqlite3_bind_text(idx->st_file_move, 3, new_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(idx->st_file_move);
    sqlite3_reset(idx->st_file_move);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "move_file");
        return SEARCH_ERR_SQLITE;
    }
    return sqlite3_changes(idx->db) > 0 ? SEARCH_OK : SEARCH_ERR_NOTFOUND;
}

int search_index_delete_file(struct search_index *idx, int64_t file_id)
{
    if (!idx || !idx->db || file_id <= 0) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (delete_doc_rows(idx, file_id) != SEARCH_OK) {
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_reset(idx->st_file_delete);
    sqlite3_clear_bindings(idx->st_file_delete);
    sqlite3_bind_int64(idx->st_file_delete, 1, file_id);
    int rc = sqlite3_step(idx->st_file_delete);
    sqlite3_reset(idx->st_file_delete);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "delete_file");
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

int search_index_delete_file_by_name(struct search_index *idx, int64_t dir_id,
                                     const char *name)
{
    if (!idx || !idx->db || dir_id <= 0 || !name || !name[0]) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    sqlite3_reset(idx->st_file_delete_name);
    sqlite3_clear_bindings(idx->st_file_delete_name);
    sqlite3_bind_int64(idx->st_file_delete_name, 1, dir_id);
    sqlite3_bind_text(idx->st_file_delete_name, 2, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(idx->st_file_delete_name);
    int64_t id = 0;
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(idx->st_file_delete_name, 0);
    } else if (rc != SQLITE_DONE) {
        set_sql_err(idx, "delete_file_by_name");
        sqlite3_reset(idx->st_file_delete_name);
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    sqlite3_reset(idx->st_file_delete_name);

    if (id > 0 && delete_doc_rows(idx, id) != SEARCH_OK) {
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

int search_index_sweep_gen(struct search_index *idx, int64_t min_gen,
                           int64_t *deleted_files, int64_t *deleted_dirs)
{
    if (!idx || !idx->db || min_gen <= 0) return SEARCH_ERR_INVAL;
    if (deleted_files) *deleted_files = 0;
    if (deleted_dirs) *deleted_dirs = 0;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    static const char *const stmts[] = {
        "DELETE FROM docs_fts WHERE rowid IN (SELECT id FROM files WHERE gen < ?1);",
        "DELETE FROM docs_uni WHERE rowid IN (SELECT id FROM files WHERE gen < ?1);",
        "DELETE FROM files WHERE gen < ?1;",
        "DELETE FROM dirs WHERE gen < ?1;"
    };

    for (size_t i = 0; i < sizeof stmts / sizeof stmts[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (prepare_stmt(idx, stmts[i], &st) != SEARCH_OK) {
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
        sqlite3_bind_int64(st, 1, min_gen);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            set_sql_err(idx, "sweep_gen");
            if (own_tx) (void)search_index_rollback(idx);
            return SEARCH_ERR_SQLITE;
        }
        if (i == 2 && deleted_files) *deleted_files = sqlite3_changes64(idx->db);
        if (i == 3 && deleted_dirs) *deleted_dirs = sqlite3_changes64(idx->db);
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    invalidate_path_cache(idx);
    return SEARCH_OK;
}

int search_index_set_text(struct search_index *idx, int64_t file_id,
                          const char *text, size_t text_len)
{
    if (!idx || !idx->db || file_id <= 0 || (!text && text_len > 0)) return SEARCH_ERR_INVAL;
    if (text_len > (size_t)INT_MAX) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    /* Mark the files row first: a vanished file (deleted while its text was
     * being extracted) must not leave orphan rows in docs_fts / docs_uni. */
    sqlite3_reset(idx->st_file_text_state);
    sqlite3_clear_bindings(idx->st_file_text_state);
    sqlite3_bind_int64(idx->st_file_text_state, 1, file_id);
    sqlite3_bind_int(idx->st_file_text_state, 2, SEARCH_TEXT_INDEXED);
    int rc = sqlite3_step(idx->st_file_text_state);
    sqlite3_reset(idx->st_file_text_state);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "set_text state");
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    if (sqlite3_changes(idx->db) == 0) {
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_NOTFOUND;
    }

    if (delete_doc_rows(idx, file_id) != SEARCH_OK) {
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    const char *body = text ? text : "";

    sqlite3_reset(idx->st_doc_fts_ins);
    sqlite3_clear_bindings(idx->st_doc_fts_ins);
    sqlite3_bind_int64(idx->st_doc_fts_ins, 1, file_id);
    sqlite3_bind_text(idx->st_doc_fts_ins, 2, body, (int)text_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(idx->st_doc_fts_ins);
    sqlite3_reset(idx->st_doc_fts_ins);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "insert docs_fts");
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_reset(idx->st_doc_uni_ins);
    sqlite3_clear_bindings(idx->st_doc_uni_ins);
    sqlite3_bind_int64(idx->st_doc_uni_ins, 1, file_id);
    sqlite3_bind_text(idx->st_doc_uni_ins, 2, body, (int)text_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(idx->st_doc_uni_ins);
    sqlite3_reset(idx->st_doc_uni_ins);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "insert docs_uni");
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

int search_index_clear_text(struct search_index *idx, int64_t file_id,
                            int new_text_state)
{
    if (!idx || !idx->db || file_id <= 0) return SEARCH_ERR_INVAL;

    int own_tx = !idx->in_tx;
    if (own_tx && search_index_begin(idx) != SEARCH_OK) return SEARCH_ERR_SQLITE;

    if (delete_doc_rows(idx, file_id) != SEARCH_OK) {
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    sqlite3_reset(idx->st_file_text_state);
    sqlite3_clear_bindings(idx->st_file_text_state);
    sqlite3_bind_int64(idx->st_file_text_state, 1, file_id);
    sqlite3_bind_int(idx->st_file_text_state, 2, new_text_state);
    int rc = sqlite3_step(idx->st_file_text_state);
    sqlite3_reset(idx->st_file_text_state);
    if (rc != SQLITE_DONE) {
        set_sql_err(idx, "clear_text state");
        if (own_tx) (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }

    if (own_tx && search_index_commit(idx) != SEARCH_OK) {
        (void)search_index_rollback(idx);
        return SEARCH_ERR_SQLITE;
    }
    return SEARCH_OK;
}

static int join_dir_and_name(const char *dir_path, const char *name,
                             char *out, size_t out_size)
{
    size_t dlen = strlen(dir_path);
    size_t nlen = strlen(name);
    int need_slash = (dlen > 0 && dir_path[dlen - 1] != '/');
    if (dlen + (size_t)need_slash + nlen + 1 > out_size) return -1;
    memcpy(out, dir_path, dlen);
    size_t pos = dlen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, name, nlen + 1);
    return 0;
}

int search_index_next_pending_docs(struct search_index *idx,
                                   struct search_pending_doc *out,
                                   size_t cap, size_t *count_out)
{
    if (!idx || !idx->db || (!out && cap > 0) || !count_out) return SEARCH_ERR_INVAL;
    *count_out = 0;
    if (cap == 0) return SEARCH_OK;

    sqlite3_reset(idx->st_pending_docs);
    sqlite3_clear_bindings(idx->st_pending_docs);
    sqlite3_bind_int64(idx->st_pending_docs, 1, (int64_t)cap);

    size_t n = 0;
    int rc;
    while ((rc = sqlite3_step(idx->st_pending_docs)) == SQLITE_ROW && n < cap) {
        int64_t file_id = sqlite3_column_int64(idx->st_pending_docs, 0);
        int64_t dir_id = sqlite3_column_int64(idx->st_pending_docs, 1);
        const char *name = (const char *)sqlite3_column_text(idx->st_pending_docs, 2);
        int64_t size = sqlite3_column_int64(idx->st_pending_docs, 3);
        int64_t mtime = sqlite3_column_int64(idx->st_pending_docs, 4);
        if (!name || !name[0]) continue;

        char dpath[SEARCH_PATH_MAX];
        if (search_index_dir_path(idx, dir_id, dpath, sizeof dpath) != SEARCH_OK) continue;
        if (join_dir_and_name(dpath, name, out[n].path, sizeof out[n].path) != 0) continue;
        out[n].file_id = file_id;
        out[n].size = size;
        out[n].mtime = mtime;
        n++;
    }
    sqlite3_reset(idx->st_pending_docs);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        set_sql_err(idx, "next_pending_docs");
        return SEARCH_ERR_SQLITE;
    }
    *count_out = n;
    return SEARCH_OK;
}

/* --- Query normalization, escaping, and snippet helpers --- */

/* Count UTF-8 codepoints (leading bytes not matching 10xxxxxx). */
static size_t utf8_codepoint_count(const char *s, size_t len)
{
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0u) != 0x80u) count++;
    }
    return count;
}

/* Trim leading/trailing ASCII whitespace and validate query constraints.
 * Returns normalized length (> 0), or 0 if empty/invalid. */
static size_t normalize_query(const char *raw, char *out, size_t out_size)
{
    if (!raw || !out || out_size == 0) return 0;
    size_t len = strnlen(raw, SEARCH_QUERY_MAX + 1);
    if (len == 0 || len > SEARCH_QUERY_MAX) return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\r' || c == '\n' || c == '\0') return 0;
    }

    size_t start = 0;
    while (start < len && isspace((unsigned char)raw[start])) start++;
    size_t end = len;
    while (end > start && isspace((unsigned char)raw[end - 1])) end--;
    size_t n = end - start;
    if (n == 0 || n >= out_size) return 0;
    memcpy(out, raw + start, n);
    out[n] = '\0';
    return n;
}

/* Build an FTS5 trigram MATCH expression safely.
 * If every whitespace-separated term has >= 3 UTF-8 codepoints, emit each term
 * as a separate double-quoted literal ("term1" "term2") so FTS5 matches them in
 * any order/separation while treating all punctuation and operators literally.
 * Otherwise emit one double-quoted phrase ("full query") so trigrams span spaces. */
static int escape_fts5_trigram(const char *q, size_t qlen, char *out, size_t out_size)
{
    if (out_size < 4) return -1;

    int multi = 0;
    int all_ge3 = 1;
    size_t i = 0;
    size_t terms = 0;
    while (i < qlen) {
        while (i < qlen && isspace((unsigned char)q[i])) i++;
        size_t start = i;
        while (i < qlen && !isspace((unsigned char)q[i])) i++;
        if (i > start) {
            terms++;
            if (utf8_codepoint_count(q + start, i - start) < 3) {
                all_ge3 = 0;
                break;
            }
        }
    }
    if (terms >= 2 && all_ge3) multi = 1;

    size_t w = 0;
    if (!multi) {
        out[w++] = '"';
        for (size_t k = 0; k < qlen; k++) {
            if (q[k] == '"') {
                if (w + 2 >= out_size) return -1;
                out[w++] = '"';
                out[w++] = '"';
            } else {
                if (w + 1 >= out_size) return -1;
                out[w++] = q[k];
            }
        }
        if (w + 2 > out_size) return -1;
        out[w++] = '"';
        out[w] = '\0';
        return 0;
    }

    i = 0;
    int emitted = 0;
    while (i < qlen) {
        while (i < qlen && isspace((unsigned char)q[i])) i++;
        size_t start = i;
        while (i < qlen && !isspace((unsigned char)q[i])) i++;
        if (i == start) continue;
        if (emitted > 0) {
            if (w + 1 >= out_size) return -1;
            out[w++] = ' ';
        }
        if (w + 1 >= out_size) return -1;
        out[w++] = '"';
        for (size_t k = start; k < i; k++) {
            if (q[k] == '"') {
                if (w + 2 >= out_size) return -1;
                out[w++] = '"';
                out[w++] = '"';
            } else {
                if (w + 1 >= out_size) return -1;
                out[w++] = q[k];
            }
        }
        if (w + 1 >= out_size) return -1;
        out[w++] = '"';
        emitted++;
    }
    if (emitted == 0 || w >= out_size) return -1;
    out[w] = '\0';
    return 0;
}

/* Build an FTS5 unicode61 prefix expression ("token"*) from whitespace-separated
 * terms in a short query. Strips ASCII punctuation that unicode61 drops as
 * separators; keeps UTF-8 multibyte bytes intact. Returns 0 if at least one
 * token was emitted, -1 if nothing searchable remained. */
static int escape_fts5_uni_prefix(const char *q, size_t qlen, char *out, size_t out_size)
{
    size_t w = 0;
    size_t i = 0;
    int tokens = 0;

    while (i < qlen) {
        while (i < qlen) {
            unsigned char c = (unsigned char)q[i];
            if (c >= 0x80u || isalnum(c)) break;
            i++;
        }
        size_t tstart = i;
        while (i < qlen) {
            unsigned char c = (unsigned char)q[i];
            if (c < 0x80u && !isalnum(c)) break;
            i++;
        }
        if (i == tstart) continue;

        if (tokens > 0) {
            if (w + 1 >= out_size) return -1;
            out[w++] = ' ';
        }
        if (w + 1 >= out_size) return -1;
        out[w++] = '"';
        for (size_t j = tstart; j < i; j++) {
            if (q[j] == '"') continue;
            if (w + 1 >= out_size) return -1;
            out[w++] = q[j];
        }
        if (w + 2 >= out_size) return -1;
        out[w++] = '"';
        out[w++] = '*';
        tokens++;
    }
    if (tokens == 0 || w >= out_size) return -1;
    out[w] = '\0';
    return 0;
}

/* Escape a query for LIKE ... ESCAPE '\', wrapped with leading and trailing %. */
static int escape_like_contains(const char *q, size_t qlen, char *out, size_t out_size)
{
    size_t w = 0;
    if (out_size < 4) return -1;
    out[w++] = '%';
    for (size_t i = 0; i < qlen; i++) {
        char c = q[i];
        if (c == '%' || c == '_' || c == '\\') {
            if (w + 2 >= out_size) return -1;
            out[w++] = '\\';
            out[w++] = c;
        } else {
            if (w + 1 >= out_size) return -1;
            out[w++] = c;
        }
    }
    if (w + 2 > out_size) return -1;
    out[w++] = '%';
    out[w] = '\0';
    return 0;
}

/* Compute an exclusive upper bound for a NOCASE prefix scan (name >= low AND
 * name < high). Works by incrementing the last byte of an ASCII-lowercased copy
 * when possible; returns 0 on success or -1 if no simple upper bound exists. */
static int make_nocase_prefix_bounds(const char *q, size_t qlen,
                                     char *low, size_t low_size,
                                     char *high, size_t high_size)
{
    if (qlen == 0 || qlen + 1 > low_size || qlen + 1 > high_size) return -1;
    for (size_t i = 0; i < qlen; i++) {
        unsigned char c = (unsigned char)q[i];
        /* SQLite's built-in NOCASE folds ASCII A-Z to a-z only. */
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        low[i] = (char)c;
        high[i] = (char)c;
    }
    low[qlen] = '\0';
    high[qlen] = '\0';

    /* Increment the final byte (for ASCII 'z', skip over '{' naturally since
     * NOCASE folds 'A'-'Z' -> 'a'-'z' and leaves '{' (0x7b) above 'z' (0x7a)). */
    for (size_t i = qlen; i > 0; i--) {
        unsigned char c = (unsigned char)high[i - 1];
        if (c < 0xFFu) {
            unsigned char next = (unsigned char)(c + 1);
            /* If incrementing landed on 'A' (0x41), NOCASE folds 'A'..'Z' into
             * 'a'..'z', so skip directly past 'Z' to '[' (0x5b). */
            if (next >= 'A' && next <= 'Z') next = '[';
            high[i - 1] = (char)next;
            high[i] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Collapse control characters, tabs, and newlines to single spaces so response
 * lines stay single-line tab-separated protocol records. Safe on UTF-8. */
static void sanitize_snippet(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t w = 0;
    int prev_sp = 1;
    for (const unsigned char *p = (const unsigned char *)src; *p && w + 1 < dst_size; p++) {
        unsigned char c = *p;
        if (c == '\t' || c == '\r' || c == '\n' || c < 0x20u || c == 0x7Fu) {
            if (!prev_sp) {
                dst[w++] = ' ';
                prev_sp = 1;
            }
        } else {
            dst[w++] = (char)c;
            prev_sp = (c == ' ');
        }
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    /* Do not leave a truncated UTF-8 multibyte sequence at the end. */
    if (w > 0 && ((unsigned char)dst[w - 1] & 0x80u) != 0) {
        size_t back = w;
        while (back > 0 && ((unsigned char)dst[back - 1] & 0xC0u) == 0x80u) back--;
        if (back > 0) {
            unsigned char lead = (unsigned char)dst[back - 1];
            size_t expect = 1;
            if ((lead & 0xE0u) == 0xC0u) expect = 2;
            else if ((lead & 0xF0u) == 0xE0u) expect = 3;
            else if ((lead & 0xF8u) == 0xF0u) expect = 4;
            if (w - (back - 1) < expect) w = back - 1;
        }
    }
    dst[w] = '\0';
}

/* Case-insensitive ASCII substring search (exact match on non-ASCII bytes). */
static const char *find_substr_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return NULL;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t j = 0;
        while (j < nlen && h[j]) {
            unsigned char a = (unsigned char)h[j];
            unsigned char b = (unsigned char)needle[j];
            if (a < 0x80u) a = (unsigned char)tolower(a);
            if (b < 0x80u) b = (unsigned char)tolower(b);
            if (a != b) break;
            j++;
        }
        if (j == nlen) return h;
    }
    return NULL;
}

static void build_window_snippet(const char *body, const char *query,
                                 char *out, size_t out_size)
{
    if (!body || !body[0]) {
        out[0] = '\0';
        return;
    }
    const char *hit = find_substr_ci(body, query);
    if (!hit) {
        /* For multi-word queries, center the snippet on the first term. */
        const char *sp = query;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        size_t tlen = (size_t)(sp - query);
        if (tlen > 0 && tlen < strlen(query) && tlen < SEARCH_QUERY_MAX) {
            char first[SEARCH_QUERY_MAX + 1];
            memcpy(first, query, tlen);
            first[tlen] = '\0';
            hit = find_substr_ci(body, first);
        }
    }
    if (!hit) {
        sanitize_snippet(body, out, out_size);
        return;
    }
    size_t offset = (size_t)(hit - body);
    size_t start = offset > 40 ? offset - 40 : 0;
    /* Advance to UTF-8 codepoint boundary. */
    while (start > 0 && ((unsigned char)body[start] & 0xC0u) == 0x80u) start++;

    char raw[SEARCH_SNIPPET_MAX + 16];
    if (start > 0) {
        snprintf(raw, sizeof raw, "...%.*s", (int)(sizeof raw - 5), body + start);
    } else {
        snprintf(raw, sizeof raw, "%.*s", (int)(sizeof raw - 1), body);
    }
    sanitize_snippet(raw, out, out_size);
}

static double score_name_match(const char *name, const char *query, size_t qlen,
                               double base_rank)
{
    /* Higher is better: exact > prefix > word-boundary > substring, adjusted by
     * FTS5 bm25 (negative in SQLite, closer to -inf is better) and shorter name. */
    double score = 100.0 - base_rank;
    size_t nlen = strlen(name);
    const char *hit = find_substr_ci(name, query);
    if (hit == name) {
        if (nlen == qlen) score += 200.0;
        else score += 100.0;
    } else if (hit != NULL) {
        char prev = hit[-1];
        if (prev == '-' || prev == '_' || prev == '.' || prev == ' ') score += 40.0;
    }
    score -= (double)nlen * 0.1;
    return score;
}

static int hit_already_present(const struct search_hit *hits, size_t n, int64_t file_id)
{
    for (size_t i = 0; i < n; i++) {
        if (hits[i].file_id == file_id) return 1;
    }
    return 0;
}

static int cmp_hits_desc(const void *a, const void *b)
{
    const struct search_hit *ha = a;
    const struct search_hit *hb = b;
    if (ha->score > hb->score) return -1;
    if (ha->score < hb->score) return 1;
    if (ha->mtime > hb->mtime) return -1;
    if (ha->mtime < hb->mtime) return 1;
    return strcmp(ha->name, hb->name);
}

static int populate_hit(struct search_index *idx, struct search_hit *h,
                        int64_t file_id, int64_t dir_id, const char *name,
                        int64_t size, int64_t mtime, int kind,
                        int layer, double score, const char *snippet)
{
    char dpath[SEARCH_PATH_MAX];
    if (search_index_dir_path(idx, dir_id, dpath, sizeof dpath) != SEARCH_OK) return -1;
    if (join_dir_and_name(dpath, name, h->path, sizeof h->path) != 0) return -1;
    h->file_id = file_id;
    h->dir_id = dir_id;
    h->size = size;
    h->mtime = mtime;
    h->layer = layer;
    h->kind = kind;
    h->score = score;
    snprintf(h->name, sizeof h->name, "%s", name);
    sanitize_snippet(snippet, h->snippet, sizeof h->snippet);
    return 0;
}

int search_index_query_names(struct search_index *idx, const char *query,
                             size_t limit, struct search_hit *out,
                             size_t *count_out)
{
    if (!idx || !idx->db || !query || !out || !count_out) return SEARCH_ERR_INVAL;
    *count_out = 0;

    char norm[SEARCH_QUERY_MAX + 1];
    size_t qlen = normalize_query(query, norm, sizeof norm);
    if (qlen == 0) return SEARCH_OK;

    if (limit == 0 || limit > SEARCH_MAX_RESULTS) limit = SEARCH_MAX_RESULTS;
    size_t chars = utf8_codepoint_count(norm, qlen);
    size_t n = 0;

    if (chars >= 3) {
        char match[SEARCH_QUERY_MAX * 2 + 16];
        if (escape_fts5_trigram(norm, qlen, match, sizeof match) != 0) return SEARCH_ERR_INVAL;

        sqlite3_reset(idx->st_query_names_fts);
        sqlite3_clear_bindings(idx->st_query_names_fts);
        sqlite3_bind_text(idx->st_query_names_fts, 1, match, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(idx->st_query_names_fts, 2, (int64_t)limit);

        int rc;
        while ((rc = sqlite3_step(idx->st_query_names_fts)) == SQLITE_ROW && n < limit) {
            int64_t file_id = sqlite3_column_int64(idx->st_query_names_fts, 0);
            int64_t dir_id = sqlite3_column_int64(idx->st_query_names_fts, 1);
            const char *name = (const char *)sqlite3_column_text(idx->st_query_names_fts, 2);
            int64_t size = sqlite3_column_int64(idx->st_query_names_fts, 3);
            int64_t mtime = sqlite3_column_int64(idx->st_query_names_fts, 4);
            int kind = sqlite3_column_int(idx->st_query_names_fts, 5);
            double rank = sqlite3_column_double(idx->st_query_names_fts, 6);
            if (!name) continue;
            double sc = score_name_match(name, norm, qlen, rank);
            if (populate_hit(idx, &out[n], file_id, dir_id, name, size, mtime,
                             kind, 1, sc, "") == 0) {
                n++;
            }
        }
        sqlite3_reset(idx->st_query_names_fts);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            set_sql_err(idx, "query_names_fts");
            return SEARCH_ERR_SQLITE;
        }
    } else {
        /* 1-2 characters: fast NOCASE prefix range scan first, then bounded LIKE scan. */
        char low[SEARCH_QUERY_MAX + 2];
        char high[SEARCH_QUERY_MAX + 2];
        if (make_nocase_prefix_bounds(norm, qlen, low, sizeof low, high, sizeof high) == 0) {
            sqlite3_reset(idx->st_query_names_prefix);
            sqlite3_clear_bindings(idx->st_query_names_prefix);
            sqlite3_bind_text(idx->st_query_names_prefix, 1, low, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(idx->st_query_names_prefix, 2, high, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(idx->st_query_names_prefix, 3, (int64_t)limit);

            int rc;
            while ((rc = sqlite3_step(idx->st_query_names_prefix)) == SQLITE_ROW && n < limit) {
                int64_t file_id = sqlite3_column_int64(idx->st_query_names_prefix, 0);
                int64_t dir_id = sqlite3_column_int64(idx->st_query_names_prefix, 1);
                const char *name = (const char *)sqlite3_column_text(idx->st_query_names_prefix, 2);
                int64_t size = sqlite3_column_int64(idx->st_query_names_prefix, 3);
                int64_t mtime = sqlite3_column_int64(idx->st_query_names_prefix, 4);
                int kind = sqlite3_column_int(idx->st_query_names_prefix, 5);
                if (!name) continue;
                double sc = score_name_match(name, norm, qlen, 0.0);
                if (populate_hit(idx, &out[n], file_id, dir_id, name, size, mtime,
                                 kind, 1, sc, "") == 0) {
                    n++;
                }
            }
            sqlite3_reset(idx->st_query_names_prefix);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                set_sql_err(idx, "query_names_prefix");
                return SEARCH_ERR_SQLITE;
            }
        }

        if (n < limit) {
            char pat[SEARCH_QUERY_MAX * 2 + 4];
            if (escape_like_contains(norm, qlen, pat, sizeof pat) != 0) return SEARCH_ERR_INVAL;

            sqlite3_reset(idx->st_query_names_like);
            sqlite3_clear_bindings(idx->st_query_names_like);
            sqlite3_bind_text(idx->st_query_names_like, 1, pat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(idx->st_query_names_like, 2, (int64_t)limit);
            sqlite3_bind_int64(idx->st_query_names_like, 3, SHORT_LIKE_SCAN);

            int rc;
            while ((rc = sqlite3_step(idx->st_query_names_like)) == SQLITE_ROW && n < limit) {
                int64_t file_id = sqlite3_column_int64(idx->st_query_names_like, 0);
                if (hit_already_present(out, n, file_id)) continue;
                int64_t dir_id = sqlite3_column_int64(idx->st_query_names_like, 1);
                const char *name = (const char *)sqlite3_column_text(idx->st_query_names_like, 2);
                int64_t size = sqlite3_column_int64(idx->st_query_names_like, 3);
                int64_t mtime = sqlite3_column_int64(idx->st_query_names_like, 4);
                int kind = sqlite3_column_int(idx->st_query_names_like, 5);
                if (!name) continue;
                double sc = score_name_match(name, norm, qlen, 5.0);
                if (populate_hit(idx, &out[n], file_id, dir_id, name, size, mtime,
                                 kind, 1, sc, "") == 0) {
                    n++;
                }
            }
            sqlite3_reset(idx->st_query_names_like);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                set_sql_err(idx, "query_names_like");
                return SEARCH_ERR_SQLITE;
            }
        }
    }

    if (n > 1) qsort(out, n, sizeof out[0], cmp_hits_desc);
    *count_out = n;
    return SEARCH_OK;
}

int search_index_query_content(struct search_index *idx, const char *query,
                               size_t limit, struct search_hit *out,
                               size_t *count_out)
{
    if (!idx || !idx->db || !query || !out || !count_out) return SEARCH_ERR_INVAL;
    *count_out = 0;

    char norm[SEARCH_QUERY_MAX + 1];
    size_t qlen = normalize_query(query, norm, sizeof norm);
    if (qlen == 0) return SEARCH_OK;

    if (limit == 0 || limit > SEARCH_MAX_RESULTS) limit = SEARCH_MAX_RESULTS;
    size_t chars = utf8_codepoint_count(norm, qlen);
    size_t n = 0;

    if (chars >= 3) {
        char match[SEARCH_QUERY_MAX * 2 + 16];
        if (escape_fts5_trigram(norm, qlen, match, sizeof match) != 0) return SEARCH_ERR_INVAL;

        sqlite3_reset(idx->st_query_docs_fts);
        sqlite3_clear_bindings(idx->st_query_docs_fts);
        sqlite3_bind_text(idx->st_query_docs_fts, 1, match, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(idx->st_query_docs_fts, 2, (int64_t)limit);

        int rc;
        while ((rc = sqlite3_step(idx->st_query_docs_fts)) == SQLITE_ROW && n < limit) {
            int64_t file_id = sqlite3_column_int64(idx->st_query_docs_fts, 0);
            int64_t dir_id = sqlite3_column_int64(idx->st_query_docs_fts, 1);
            const char *name = (const char *)sqlite3_column_text(idx->st_query_docs_fts, 2);
            int64_t size = sqlite3_column_int64(idx->st_query_docs_fts, 3);
            int64_t mtime = sqlite3_column_int64(idx->st_query_docs_fts, 4);
            int kind = sqlite3_column_int(idx->st_query_docs_fts, 5);
            double rank = sqlite3_column_double(idx->st_query_docs_fts, 6);
            const char *body_preview = (const char *)sqlite3_column_text(idx->st_query_docs_fts, 7);
            if (!name) continue;

            char snip[SEARCH_SNIPPET_MAX];
            build_window_snippet(body_preview ? body_preview : "", norm, snip, sizeof snip);
            double sc = -rank;
            if (populate_hit(idx, &out[n], file_id, dir_id, name, size, mtime,
                             kind, 2, sc, snip) == 0) {
                n++;
            }
        }
        sqlite3_reset(idx->st_query_docs_fts);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            set_sql_err(idx, "query_docs_fts");
            return SEARCH_ERR_SQLITE;
        }
    } else {
        char match[SEARCH_QUERY_MAX * 2 + 16];
        if (escape_fts5_uni_prefix(norm, qlen, match, sizeof match) != 0) {
            /* Punctuation-only short query has no unicode61 tokens. */
            return SEARCH_OK;
        }

        sqlite3_reset(idx->st_query_docs_uni);
        sqlite3_clear_bindings(idx->st_query_docs_uni);
        sqlite3_bind_text(idx->st_query_docs_uni, 1, match, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(idx->st_query_docs_uni, 2, (int64_t)limit);

        int rc;
        while ((rc = sqlite3_step(idx->st_query_docs_uni)) == SQLITE_ROW && n < limit) {
            int64_t file_id = sqlite3_column_int64(idx->st_query_docs_uni, 0);
            int64_t dir_id = sqlite3_column_int64(idx->st_query_docs_uni, 1);
            const char *name = (const char *)sqlite3_column_text(idx->st_query_docs_uni, 2);
            int64_t size = sqlite3_column_int64(idx->st_query_docs_uni, 3);
            int64_t mtime = sqlite3_column_int64(idx->st_query_docs_uni, 4);
            int kind = sqlite3_column_int(idx->st_query_docs_uni, 5);
            double rank = sqlite3_column_double(idx->st_query_docs_uni, 6);
            const char *snip = (const char *)sqlite3_column_text(idx->st_query_docs_uni, 7);
            if (!name) continue;

            double sc = -rank;
            if (populate_hit(idx, &out[n], file_id, dir_id, name, size, mtime,
                             kind, 2, sc, snip ? snip : "") == 0) {
                n++;
            }
        }
        sqlite3_reset(idx->st_query_docs_uni);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            set_sql_err(idx, "query_docs_uni");
            return SEARCH_ERR_SQLITE;
        }
    }

    *count_out = n;
    return SEARCH_OK;
}

size_t search_index_filter_ghosts(struct search_hit *hits, size_t *count,
                                  size_t top_n,
                                  int64_t *gone_ids, size_t gone_cap,
                                  size_t *gone_count)
{
    if (gone_count) *gone_count = 0;
    if (!hits || !count || *count == 0) return 0;

    size_t total = *count;
    size_t check_limit = (top_n == 0 || top_n > total) ? total : top_n;
    size_t w = 0;
    size_t dropped = 0;
    size_t recorded = 0;

    for (size_t i = 0; i < total; i++) {
        int gone = 0;
        if (i < check_limit) {
            if (faccessat(AT_FDCWD, hits[i].path, F_OK, 0) != 0) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    gone = 1;
                }
            }
        }
        if (gone) {
            dropped++;
            if (gone_ids && recorded < gone_cap) {
                gone_ids[recorded++] = hits[i].file_id;
            }
            continue;
        }
        if (w != i) hits[w] = hits[i];
        w++;
    }

    *count = w;
    if (gone_count) *gone_count = recorded;
    return dropped;
}

int search_index_incremental_vacuum(struct search_index *idx, int pages)
{
    if (!idx || !idx->db || pages < 0) return SEARCH_ERR_INVAL;
    if (idx->in_tx) return SEARCH_ERR;
    char sql[64];
    if (pages == 0) {
        snprintf(sql, sizeof sql, "PRAGMA incremental_vacuum;");
    } else {
        snprintf(sql, sizeof sql, "PRAGMA incremental_vacuum(%d);", pages);
    }
    return exec_sql(idx, sql);
}
