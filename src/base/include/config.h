/* Shared configuration transactions; C, Python and the shell client use the
 * same path.lock and flock protocol. See rc.c and link/locachy_config.py. */
#ifndef LOCACHY_CONFIG_H
#define LOCACHY_CONFIG_H

#include <stddef.h>

struct conf_change {
    const char *name;
    const char *value; /* NULL removes every occurrence of name */
};

/* Paths are absolute. The existing parent is canonicalized; the last component
 * must not be a symlink. path.lock is a stable, private regular file, never
 * unlinked, even on failure. Locks cover the complete read/modify/publish cycle.
 * Lock order: a caller's domain lock, then the configuration lock. Callbacks
 * must not reacquire this lock, take domain locks, wait for jobs or prompt.
 *
 * Existing mode/uid/gid are preserved; new files are 0644, owned by the caller.
 * Publication uses a unique same-directory temporary, file fsync, rename and
 * directory fsync. A failure before rename preserves the old file. A directory
 * fsync failure after rename reports failure, but the new complete file may
 * already be published: retry/re-read, never assume a rollback.
 *
 * These are cooperating-writer locks, not protection against arbitrary writes.
 * Changed file identity/metadata at commit is refused with ESTALE. */

/* A callback sees all original bytes (NUL-terminated for convenience; length
 * excludes the terminator), and whether the file existed. Return 0 with a
 * malloc-owned replacement/length (the transaction frees it), 1 for unchanged,
 * or -1 with errno. An empty replacement may be NULL. Never return an alias of
 * original. conf_transaction returns 0 on commit/unchanged, -1 on failure. */
typedef int (*conf_transform)(const char *original, size_t length, int exists,
                              char **replacement, size_t *replacement_length, void *arg);
int conf_transaction(const char *path, conf_transform transform, void *arg);

/* Batch NAME="VALUE" updates, preserving unrelated lines/comments and order.
 * Names are nonempty ASCII letters/digits/underscore. Values contain no quote,
 * CR or LF. Duplicate names in a batch are refused; duplicate lines in the old
 * file become one. A missing final newline is repaired before appending. A
 * batch of removals on an absent file creates nothing. */
int conf_update(const char *path, const struct conf_change *changes, size_t count);
int rc_update(const struct conf_change *changes, size_t count);

/* Optimistic versions: expected == NULL means the file must not exist; a
 * non-NULL expected (even length zero) must match all bytes of an existing
 * file. A mismatch returns -1/ESTALE without writing. */
int conf_compare_update(const char *path, const char *expected, size_t expected_length,
                        const struct conf_change *changes, size_t count);
int conf_compare_exchange(const char *path, const char *expected, size_t expected_length,
                          const char *replacement, size_t replacement_length);

#endif
