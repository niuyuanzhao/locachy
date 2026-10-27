/* Asynchronous Find in Files using the editor's PCRE2 search semantics.
 * The worker reads files as EDIT_USER, never with the screen owner's access. */
#ifndef LOCACHY_EDIT_FILESEARCH_H
#define LOCACHY_EDIT_FILESEARCH_H
#include <stddef.h>
#include <stdint.h>

/* A source with NULL text reads disk; non-NULL text is an immutable open-buffer
 * snapshot. Duplicate paths select a buffer over disk. Inputs need only remain
 * valid for start(): fork gives the worker its own snapshot. */
struct edit_search_source { const char *path, *text; size_t length; };
struct edit_filesearch_options {
    const char *root, *pattern, *include, *exclude;
    int mode, match_case, recursive, hidden, follow_links;
    /* Open-document scope: buffers take precedence over disk. NULL/zero means
     * enumerate root. A non-NULL empty array represents an empty scope. */
    const struct edit_search_source *sources;
    size_t nsources;
    /* Folder/project scope still enumerates root, but matching paths use these
     * unsaved UTF-8 buffer snapshots instead of disk. Does not widen the scope;
     * hidden, links, recursion and globs remain enumeration choices. Paths are
     * compared as normalized absolute paths (without resolving symlinks). */
    const struct edit_search_source *overlays;
    size_t noverlays;
};
struct edit_filematch {
    char *path, *preview;
    size_t start, end;               /* normalized UTF-8 byte range [start,end) */
    size_t line, column;             /* 1-based; column counts Unicode codepoints */
    uint8_t digest[20];              /* disk snapshot for checked replacement */
    int checked, from_buffer;
    uint8_t text_digest[20];         /* normalized UTF-8 snapshot, also for buffers */
};
struct edit_filesearch;
struct edit_filesearch *edit_filesearch_start(const struct edit_filesearch_options *, char *error, size_t size);
int edit_filesearch_fd(const struct edit_filesearch *); /* -1 once done/reaped */
/* Drain available results without waiting: 1 changed, 0 none, -1 failed.
 * Stored results remain owned by the search until it is freed. */
int edit_filesearch_read(struct edit_filesearch *);
int edit_filesearch_done(const struct edit_filesearch *);
const char *edit_filesearch_error(const struct edit_filesearch *);
size_t edit_filesearch_count(const struct edit_filesearch *);
struct edit_filematch *edit_filesearch_match(struct edit_filesearch *, size_t index);
void edit_filesearch_free(struct edit_filesearch *); /* kill + reap worker if active */

/* Globs are separated by semicolons or commas (whitespace is literal). A glob
 * without '/' matches basenames; one with '/' matches root-relative paths.
 * Excluded directory names or paths (with or without trailing '/') prune them.
 * Decoded NULs, non-whitespace C0 controls, and broken encodings are binary:
 * disk files containing them are skipped. Open buffers are searched verbatim.
 * Files over 64 MiB and searches over 100000 matches / 64 MiB of results stop
 * or skip with a visible diagnostic. Paths are never silently truncated. */
struct edit_filesearch_stats { size_t files, binary, skipped, errors; };
struct edit_filesearch_stats edit_filesearch_stats(const struct edit_filesearch *);
const char *edit_filesearch_warning(const struct edit_filesearch *);

#include "edit-file.h"
struct edit_filesearch_plan {
    char *path, *text;
    size_t length, replacements;
    struct file_format format;       /* keep this for file_save; buffers keep theirs */
    uint8_t digest[20], text_digest[20]; /* original disk and normalized snapshots */
    int from_buffer;
};
/* Prepare checked matches for exactly one path, after UI confirmation. The
 * search must be finished successfully. For open buffers pass CURRENT text;
 * for disk pass NULL. Rechecks snapshot and match ranges, uses the original
 * PCRE2 pattern/captures, returns new UTF-8 text without touching buffer/disk.
 * 0 success, errno otherwise. Explicit one-file preparation may wait for IO;
 * start/read/cancel never wait for a scan. A plan owns its text and path. */
int edit_filesearch_prepare(struct edit_filesearch *, const char *path, const char *replacement,
                            const char *current, size_t length, struct edit_filesearch_plan *, char *error, size_t size);
/* Recheck immediately before applying the plan via file_save or a buffer edit.
 * Returns ESTALE if disk/buffer changed since the search. This does not lock a
 * file against concurrent writers; callers retain their normal save conflict
 * checks. Disk IO is done by a permanently unprivileged short-lived worker. */
int edit_filesearch_plan_validate(const struct edit_filesearch_plan *, const char *current, size_t length);
void edit_filesearch_plan_free(struct edit_filesearch_plan *);
#endif
