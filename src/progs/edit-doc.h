/* edit's documents (edit-doc.c): the text, its file and format, its syntax
 * highlighting, and undo and redo as KTextEditor's undo manager groups them.
 * Positions are byte offsets of the text. */
#ifndef LOCACHY_EDIT_DOC_H
#define LOCACHY_EDIT_DOC_H

#include <limits.h>
#include <stddef.h>
#include <sys/stat.h>

#include "edit-buffer.h"
#include "edit-file.h"
#include "edit-syntax.h"

enum { UNDO_INSERT, UNDO_REMOVE, UNDO_MANY };

struct undo_item {
    int type;
    size_t at, len;           /* UNDO_INSERT, UNDO_REMOVE: len bytes of text at at */
    char *text;
    struct replacement *r;    /* UNDO_MANY: what was replaced, in the text after it, */
    size_t n;                 /* with the text replaced in each r[i].text */
};

struct undo_group {
    struct undo_item *items;
    size_t n, size;
    size_t cursor_before, anchor_before, cursor_after, anchor_after;
    int safe;                 /* a safe point: nothing more merges into it */
};

struct doc {
    struct buffer buf;
    char path[PATH_MAX];      /* "" while it has none (Untitled) */
    int untitled;             /* its number among the documents without a path */
    struct file_format format;
    int flags;                /* FILE_BROKEN, FILE_WRAPPED */
    int readonly;
    int overwrite;            /* Insert's mode: a document's setting in KTextEditor */
    int auto_reload;          /* the prompt's Enable Auto Reload */
    int lang;                 /* syntax_detect's, or -1 */
    struct syntax *syntax;
    char syntax_why[640];     /* why it has no highlighting */
    struct undo_group *groups;
    size_t done, count, size; /* groups [0, done) are done, [done, count) can be redone */
    long saved;               /* done when it was last saved (or loaded); -1: none */
    struct undo_group *open;  /* the edit being made (doc_begin .. doc_end) */
    int merge;                /* the next edit may merge into the last group */
    struct stat st;           /* the file as last read or written (st_ino 0: none) */
    uint8_t digest[DIGEST_SIZE]; /* of its bytes then (KTextEditor's checksum) */
    int watch;                /* inotify's (of its folder), or -1 */
    int disk, disk_shown;     /* DISK_*: what happened to it, and what was last asked */
    /* Called for each change of the text, to move the positions of views. */
    void (*moved)(struct doc *d, size_t at, size_t removed, size_t inserted);
    void *ui;                 /* the program's own about it */
};

enum { DISK_NONE, DISK_MODIFIED, DISK_CREATED, DISK_DELETED };

int doc_init(struct doc *d);
void doc_free(struct doc *d);
/* The text replaced, undo history cleared and the document unmodified. */
int doc_set_text(struct doc *d, const char *text, size_t len);
/* The same with text (malloc'd) taken over. */
int doc_take_text(struct doc *d, char *text, size_t len);
int doc_modified(const struct doc *d);
void doc_mark_saved(struct doc *d);
/* Language and highlighting for the document's name and text. */
void doc_detect_syntax(struct doc *d);

/* An edit: doc_begin, any insertions and removals, doc_end with where the
 * cursor and selection anchor are then. Outside one they are an edit each. */
void doc_begin(struct doc *d, size_t cursor, size_t anchor);
int doc_insert(struct doc *d, size_t at, const char *text, size_t len);
void doc_remove(struct doc *d, size_t at, size_t len);
/* The replacements of replace all, one undo step; r and its texts are
 * the document's after it. */
int doc_replace_many(struct doc *d, struct replacement *r, size_t n);
void doc_end(struct doc *d, size_t cursor, size_t anchor);
/* Kate's undoSafePoint: the next edit starts a group of its own. */
void doc_safe_point(struct doc *d);
/* 1 and the cursor and anchor to show, or 0 if there is nothing to undo. */
int doc_undo(struct doc *d, size_t *cursor, size_t *anchor);
int doc_redo(struct doc *d, size_t *cursor, size_t *anchor);

/* Line and the byte in it of a position. */
void doc_line_col(const struct doc *d, size_t at, size_t *line, size_t *col);

#endif
