/*
 * test-search-index.c: unit tests for the portable SQLite/FTS5 search engine
 * (progs/search-index.h, progs/search-index.c).
 *
 * Uses a temporary directory created via mkdtemp under $CLAUDE_JOB_DIR/tmp
 * (falling back to /tmp) and removes all temporary files on exit.
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "search-index.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
static char g_tmpdir[256];

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void make_tmpdir(void)
{
    const char *job = getenv("CLAUDE_JOB_DIR");
    if (job && job[0]) {
        char base[200];
        int n = snprintf(base, sizeof base, "%s/tmp", job);
        if (n > 0 && (size_t)n < sizeof base) {
            (void)mkdir(base, 0700);
            n = snprintf(g_tmpdir, sizeof g_tmpdir, "%s/search-idx-test-XXXXXX", base);
            if (n > 0 && (size_t)n < sizeof g_tmpdir && mkdtemp(g_tmpdir) != NULL) {
                return;
            }
        }
    }
    snprintf(g_tmpdir, sizeof g_tmpdir, "/tmp/search-idx-test-XXXXXX");
    if (!mkdtemp(g_tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
}

static void remove_db_files(const char *db_path)
{
    char p[PATH_MAX];
    (void)unlink(db_path);
    snprintf(p, sizeof p, "%s-wal", db_path);
    (void)unlink(p);
    snprintf(p, sizeof p, "%s-shm", db_path);
    (void)unlink(p);
}

static int query_pragma_str(const char *db_path, const char *sql, char *out, size_t out_size)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return -1;
    sqlite3_stmt *st = NULL;
    int ok = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(st, 0);
            if (s) {
                snprintf(out, out_size, "%s", s);
                ok = 0;
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

static int query_count(const char *db_path, const char *sql, int64_t *out)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return -1;
    sqlite3_stmt *st = NULL;
    int ok = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            *out = sqlite3_column_int64(st, 0);
            ok = 0;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

static int has_hit_id(const struct search_hit *hits, size_t n, int64_t fid)
{
    for (size_t i = 0; i < n; i++) {
        if (hits[i].file_id == fid) return 1;
    }
    return 0;
}

static void test_pragmas_and_schema(void)
{
    char db_path[PATH_MAX];
    snprintf(db_path, sizeof db_path, "%s/schema.db", g_tmpdir);

    struct search_index *idx = NULL;
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    CHECK(idx != NULL);
    search_index_close(idx);

    char val[64] = "";
    CHECK(query_pragma_str(db_path, "PRAGMA journal_mode;", val, sizeof val) == 0);
    CHECK(strcmp(val, "wal") == 0);

    int64_t av = -1;
    CHECK(query_count(db_path, "PRAGMA auto_vacuum;", &av) == 0);
    CHECK(av == 2); /* 2 == INCREMENTAL */

    int64_t uv = -1;
    CHECK(query_count(db_path, "PRAGMA user_version;", &uv) == 0);
    CHECK(uv == SEARCH_SCHEMA_VERSION);

    /* Reopen existing valid DB succeeds. */
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    search_index_close(idx);

    /* Incompatible future schema version is refused. */
    sqlite3 *raw = NULL;
    CHECK(sqlite3_open(db_path, &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "PRAGMA user_version=99;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    CHECK(search_index_open(db_path, &idx) == SEARCH_ERR_SCHEMA);
    CHECK(idx == NULL);

    remove_db_files(db_path);

    /* Foreign database (tables present with user_version=0) is refused. */
    snprintf(db_path, sizeof db_path, "%s/foreign.db", g_tmpdir);
    CHECK(sqlite3_open(db_path, &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "CREATE TABLE other(x INT);", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    CHECK(search_index_open(db_path, &idx) == SEARCH_ERR_SCHEMA);
    CHECK(idx == NULL);
    remove_db_files(db_path);
}

static void test_dirs_moves_and_subtrees(void)
{
    char db_path[PATH_MAX];
    snprintf(db_path, sizeof db_path, "%s/dirs.db", g_tmpdir);

    struct search_index *idx = NULL;
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    CHECK(search_index_begin(idx) == SEARCH_OK);
    CHECK(search_index_in_tx(idx) == 1);

    int64_t gen = search_index_next_gen(idx);
    CHECK(gen == 1);
    int64_t root = search_index_upsert_dir(idx, 0, "/home/alice", 100, gen);
    int64_t docs = search_index_upsert_dir(idx, root, "Documents", 101, gen);
    int64_t work = search_index_upsert_dir(idx, docs, "Work", 102, gen);
    int64_t arch = search_index_upsert_dir(idx, root, "Archive", 103, gen);
    CHECK(root > 0 && docs > 0 && work > 0 && arch > 0);

    int64_t f1 = search_index_upsert_file(idx, work, "quarterly-report.pdf", 4096, 200,
                                          SEARCH_KIND_DOC, gen, SEARCH_TEXT_PENDING);
    int64_t f2 = search_index_upsert_file(idx, work, "notes.md", 128, 201,
                                          SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING);
    CHECK(f1 > 0 && f2 > 0);
    CHECK(search_index_set_text(idx, f1, "alpha beta quarterly revenue summary", 36) == SEARCH_OK);
    CHECK(search_index_set_text(idx, f2, "meeting notes for architecture review", 37) == SEARCH_OK);
    CHECK(search_index_commit(idx) == SEARCH_OK);

    char path[SEARCH_PATH_MAX];
    CHECK(search_index_dir_path(idx, work, path, sizeof path) == SEARCH_OK);
    CHECK(strcmp(path, "/home/alice/Documents/Work") == 0);

    /* Moving a directory into its own descendant must be refused. */
    CHECK(search_index_move_dir(idx, docs, work, "BadMove") == SEARCH_ERR_INVAL);

    /* Move Work under Archive with one UPDATE and verify path cache updates. */
    CHECK(search_index_move_dir(idx, work, arch, "Work2026") == SEARCH_OK);
    CHECK(search_index_dir_path(idx, work, path, sizeof path) == SEARCH_OK);
    CHECK(strcmp(path, "/home/alice/Archive/Work2026") == 0);

    /* Move a file and verify names_fts reflects the new name and drops the old. */
    CHECK(search_index_move_file(idx, f2, arch, "roadmap.md") == SEARCH_OK);

    struct search_hit hits[16];
    size_t count = 0;
    CHECK(search_index_query_names(idx, "roadmap", 16, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f2);
    CHECK(strcmp(hits[0].path, "/home/alice/Archive/roadmap.md") == 0);

    CHECK(search_index_query_names(idx, "notes", 16, hits, &count) == SEARCH_OK);
    CHECK(count == 0);

    /* Delete Archive subtree recursively; verify dirs, files, names_fts, docs_fts, docs_uni are empty. */
    CHECK(search_index_delete_subtree(idx, arch) == SEARCH_OK);
    CHECK(search_index_query_names(idx, "quarterly", 16, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "revenue", 16, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "re", 16, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_dir_path(idx, work, path, sizeof path) == SEARCH_ERR_NOTFOUND);

    search_index_close(idx);

    int64_t nfiles = -1, ndocs_uni = -1;
    CHECK(query_count(db_path, "SELECT count(*) FROM files;", &nfiles) == 0 && nfiles == 0);
    CHECK(query_count(db_path, "SELECT count(*) FROM docs_uni;", &ndocs_uni) == 0 && ndocs_uni == 0);
    remove_db_files(db_path);
}

static void test_queries_and_escaping(void)
{
    char db_path[PATH_MAX];
    snprintf(db_path, sizeof db_path, "%s/queries.db", g_tmpdir);

    struct search_index *idx = NULL;
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    CHECK(search_index_begin(idx) == SEARCH_OK);

    int64_t gen = search_index_next_gen(idx);
    int64_t root = search_index_upsert_dir(idx, 0, "/home/alice", 10, gen);
    int64_t f_exact = search_index_upsert_file(idx, root, "Makefile", 100, 500,
                                               SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING);
    int64_t f_pref  = search_index_upsert_file(idx, root, "Makefile.local", 100, 400,
                                               SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING);
    int64_t f_sub   = search_index_upsert_file(idx, root, "backup_Makefile_old", 100, 600,
                                               SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING);
    int64_t f_cjk   = search_index_upsert_file(idx, root, "\xe8\xae\xa1\xe5\x88\x92\xe6\x96\x87\xe6\xa1\xa3.md",
                                               200, 550, SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING); /* 计划文档.md */
    int64_t f_quote = search_index_upsert_file(idx, root, "a\"b_100%_test.txt", 90, 510,
                                               SEARCH_KIND_TEXT, gen, SEARCH_TEXT_PENDING);
    CHECK(f_exact > 0 && f_pref > 0 && f_sub > 0 && f_cjk > 0 && f_quote > 0);

    const char *body_en = "First line\twith\ttabs\nSecond line mentions trigonometry and quantum mechanics.";
    const char *body_cjk = "\xe4\xb8\xad\xe6\x96\x87\xe5\x85\xa8\xe6\x96\x87\xe6\xa3\x80\xe7\xb4\xa2\xe6\xb5\x8b\xe8\xaf\x95 "
                           "and short prefix xyalpha keyword."; /* 中文全文检索测试 */
    CHECK(search_index_set_text(idx, f_exact, body_en, strlen(body_en)) == SEARCH_OK);
    CHECK(search_index_set_text(idx, f_cjk, body_cjk, strlen(body_cjk)) == SEARCH_OK);
    CHECK(search_index_commit(idx) == SEARCH_OK);

    struct search_hit hits[SEARCH_MAX_RESULTS];
    size_t count = 0;

    /* 3+ char name query: exact match ranks ahead of prefix and substring. */
    CHECK(search_index_query_names(idx, "Makefile", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 3);
    if (count >= 3) {
        CHECK(hits[0].file_id == f_exact);
        CHECK(hits[1].file_id == f_pref);
        CHECK(hits[2].file_id == f_sub);
        CHECK(hits[0].layer == 1);
    }

    /* 1-2 char name query: prefix + bounded LIKE fallback. */
    CHECK(search_index_query_names(idx, "ma", 20, hits, &count) == SEARCH_OK);
    CHECK(count >= 3);
    CHECK(has_hit_id(hits, count, f_exact));
    CHECK(has_hit_id(hits, count, f_pref));
    CHECK(has_hit_id(hits, count, f_sub));

    /* Short 2-char CJK name query. */
    CHECK(search_index_query_names(idx, "\xe8\xae\xa1\xe5\x88\x92", 20, hits, &count) == SEARCH_OK); /* 计划 */
    CHECK(count == 1 && hits[0].file_id == f_cjk);

    /* 3+ char content query via docs_fts trigram (substring + multi-word + CJK). */
    CHECK(search_index_query_content(idx, "trigon", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_exact && hits[0].layer == 2);
    CHECK(strchr(hits[0].snippet, '\t') == NULL && strchr(hits[0].snippet, '\n') == NULL);

    CHECK(search_index_query_content(idx, "quantum trigonometry", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_exact);

    CHECK(search_index_query_content(idx, "\xe5\x85\xa8\xe6\x96\x87\xe6\xa3\x80", 20, hits, &count) == SEARCH_OK); /* 全文检 */
    CHECK(count == 1 && hits[0].file_id == f_cjk);

    /* 1-2 char content query via docs_uni unicode61 prefix match. */
    CHECK(search_index_query_content(idx, "xy", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_cjk);

    /* Update/replace content on same rowid (verifies contentless_delete=1). */
    const char *body_v2 = "Completely replaced document body about astronomy.";
    CHECK(search_index_set_text(idx, f_exact, body_v2, strlen(body_v2)) == SEARCH_OK);
    CHECK(search_index_query_content(idx, "trigon", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "astronomy", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_exact);

    /* SQL/FTS5 injection and special characters must not error or leak rows. */
    CHECK(search_index_query_names(idx, "100%", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_quote);
    CHECK(search_index_query_names(idx, "a\"b", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == f_quote);
    CHECK(search_index_query_names(idx, "\" OR 1=1; DROP TABLE files;--", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "NEAR(a b) \" OR * -- ; DROP TABLE dirs;", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "\"'", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 0);

    /* Verify files table survived injection probes intact. */
    CHECK(search_index_query_names(idx, "Makefile", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 3);

    search_index_close(idx);
    remove_db_files(db_path);
}

static void test_generation_sweep_and_cap(void)
{
    char db_path[PATH_MAX];
    snprintf(db_path, sizeof db_path, "%s/gen_cap.db", g_tmpdir);

    struct search_index *idx = NULL;
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    CHECK(search_index_begin(idx) == SEARCH_OK);

    int64_t gen1 = search_index_next_gen(idx);
    int64_t root = search_index_upsert_dir(idx, 0, "/home/bob", 1, gen1);
    int64_t stale_dir = search_index_upsert_dir(idx, root, "old_dir", 1, gen1);
    int64_t stale_file = search_index_upsert_file(idx, stale_dir, "vanished_item.txt",
                                                  50, 10, SEARCH_KIND_TEXT, gen1, SEARCH_TEXT_PENDING);
    CHECK(search_index_set_text(idx, stale_file, "vanished secret phrase", 22) == SEARCH_OK);

    /* Insert 250 matching files to verify the 200-result cap. */
    for (int i = 0; i < 250; i++) {
        char fname[64];
        snprintf(fname, sizeof fname, "matchtoken_%03d.txt", i);
        int64_t fid = search_index_upsert_file(idx, root, fname, 100, 1000 + i,
                                               SEARCH_KIND_TEXT, gen1, SEARCH_TEXT_PENDING);
        CHECK(fid > 0);
        if (i < 210) {
            CHECK(search_index_set_text(idx, fid, "commonbodyneedle text", 21) == SEARCH_OK);
        }
    }
    CHECK(search_index_commit(idx) == SEARCH_OK);

    /* Query asking for 500 results must clamp to SEARCH_MAX_RESULTS (200). */
    struct search_hit *hits = calloc(SEARCH_MAX_RESULTS, sizeof *hits);
    CHECK(hits != NULL);
    size_t count = 0;
    CHECK(search_index_query_names(idx, "matchtoken", 500, hits, &count) == SEARCH_OK);
    CHECK(count == SEARCH_MAX_RESULTS);
    CHECK(search_index_query_content(idx, "commonbodyneedle", 500, hits, &count) == SEARCH_OK);
    CHECK(count == SEARCH_MAX_RESULTS);

    /* Pending docs query returns files still in PENDING/STALE state (40 of 250). */
    struct search_pending_doc pending[64];
    size_t npend = 0;
    CHECK(search_index_next_pending_docs(idx, pending, 64, &npend) == SEARCH_OK);
    CHECK(npend == 40);

    /* Generation 2 touches only root and matchtoken_000..009; sweep removes the rest. */
    CHECK(search_index_begin(idx) == SEARCH_OK);
    int64_t gen2 = search_index_next_gen(idx);
    CHECK(gen2 == gen1 + 1);
    CHECK(search_index_upsert_dir(idx, 0, "/home/bob", 2, gen2) == root);
    for (int i = 0; i < 10; i++) {
        char fname[64];
        snprintf(fname, sizeof fname, "matchtoken_%03d.txt", i);
        /* Unchanged (mtime, size) must keep INDEXED text_state intact. */
        int64_t fid = search_index_upsert_file(idx, root, fname, 100, 1000 + i,
                                               SEARCH_KIND_TEXT, gen2, SEARCH_TEXT_PENDING);
        CHECK(fid > 0);
    }
    int64_t df = 0, dd = 0;
    CHECK(search_index_sweep_gen(idx, gen2, &df, &dd) == SEARCH_OK);
    CHECK(df == 241); /* 240 matchtokens + 1 vanished_item.txt */
    CHECK(dd == 1);   /* old_dir */
    CHECK(search_index_commit(idx) == SEARCH_OK);

    CHECK(search_index_incremental_vacuum(idx, 32) == SEARCH_OK);

    CHECK(search_index_query_names(idx, "matchtoken", 200, hits, &count) == SEARCH_OK);
    CHECK(count == 10);
    CHECK(search_index_query_content(idx, "vanished", 20, hits, &count) == SEARCH_OK);
    CHECK(count == 0);
    CHECK(search_index_query_content(idx, "commonbodyneedle", 200, hits, &count) == SEARCH_OK);
    CHECK(count == 10);

    /* Modified file transitions INDEXED -> STALE and shows up in pending docs. */
    CHECK(search_index_upsert_file(idx, root, "matchtoken_000.txt", 150, 9999,
                                   SEARCH_KIND_TEXT, gen2, SEARCH_TEXT_PENDING) > 0);
    CHECK(search_index_next_pending_docs(idx, pending, 64, &npend) == SEARCH_OK);
    CHECK(npend == 1);

    free(hits);
    search_index_close(idx);
    remove_db_files(db_path);
}

static void test_ghost_filter_and_unwatched_prune(void)
{
    char fs_root[512];
    snprintf(fs_root, sizeof fs_root, "%s/files", g_tmpdir);
    CHECK(mkdir(fs_root, 0700) == 0);

    char db_path[PATH_MAX];
    snprintf(db_path, sizeof db_path, "%s/ghost.db", g_tmpdir);

    struct search_index *idx = NULL;
    CHECK(search_index_open(db_path, &idx) == SEARCH_OK);
    CHECK(search_index_begin(idx) == SEARCH_OK);
    int64_t gen = search_index_next_gen(idx);
    int64_t dir = search_index_upsert_dir(idx, 0, fs_root, 1, gen);

    int64_t ids[25];
    for (int i = 0; i < 25; i++) {
        char name[64], fpath[PATH_MAX];
        snprintf(name, sizeof name, "ghostcase_%02d.txt", i);
        snprintf(fpath, sizeof fpath, "%s/%s", fs_root, name);
        int fd = open(fpath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        CHECK(fd >= 0);
        if (fd >= 0) close(fd);
        ids[i] = search_index_upsert_file(idx, dir, name, 0, 1000 - i,
                                          SEARCH_KIND_TEXT, gen, SEARCH_TEXT_SKIPPED);
        CHECK(ids[i] > 0);
    }
    CHECK(search_index_commit(idx) == SEARCH_OK);

    /* Delete two files inside top 20 and one file after top 20. */
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/ghostcase_00.txt", fs_root); CHECK(unlink(p) == 0);
    snprintf(p, sizeof p, "%s/ghostcase_05.txt", fs_root); CHECK(unlink(p) == 0);
    snprintf(p, sizeof p, "%s/ghostcase_24.txt", fs_root); CHECK(unlink(p) == 0);

    struct search_hit hits[32];
    size_t count = 0;
    CHECK(search_index_query_names(idx, "ghostcase", 32, hits, &count) == SEARCH_OK);
    CHECK(count == 25);

    int64_t gone[20];
    size_t ngone = 0;
    size_t dropped = search_index_filter_ghosts(hits, &count, 20, gone, 20, &ngone);
    CHECK(dropped == 2);
    CHECK(ngone == 2);
    CHECK(count == 23);
    CHECK(!has_hit_id(hits, count, ids[0]));
    CHECK(!has_hit_id(hits, count, ids[5]));
    /* Entry beyond top 20 was not probed by top_n=20 filter. */
    CHECK(has_hit_id(hits, count, ids[24]));

    /* Single-directory stale mark/prune (used by 10-minute unwatched sweep). */
    CHECK(search_index_begin(idx) == SEARCH_OK);
    CHECK(search_index_mark_dir_children_stale(idx, dir) == SEARCH_OK);
    CHECK(search_index_upsert_file(idx, dir, "ghostcase_01.txt", 0, 999,
                                   SEARCH_KIND_TEXT, gen, SEARCH_TEXT_SKIPPED) == ids[1]);
    CHECK(search_index_prune_dir_stale(idx, dir) == SEARCH_OK);
    CHECK(search_index_commit(idx) == SEARCH_OK);

    CHECK(search_index_query_names(idx, "ghostcase", 32, hits, &count) == SEARCH_OK);
    CHECK(count == 1 && hits[0].file_id == ids[1]);

    search_index_close(idx);
    remove_db_files(db_path);

    for (int i = 0; i < 25; i++) {
        snprintf(p, sizeof p, "%s/ghostcase_%02d.txt", fs_root, i);
        (void)unlink(p);
    }
    (void)rmdir(fs_root);
}

int main(void)
{
    make_tmpdir();
    test_pragmas_and_schema();
    test_dirs_moves_and_subtrees();
    test_queries_and_escaping();
    test_generation_sweep_and_cap();
    test_ghost_filter_and_unwatched_prune();
    (void)rmdir(g_tmpdir);

    if (g_failures > 0) {
        fprintf(stderr, "test-search-index: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("PASS test-search-index");
    return 0;
}
