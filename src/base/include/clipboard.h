/* The host's current clipboard, independent of a Wayland session. The local
 * socket is root-only and is never mounted into a container. Payloads are
 * sealed memfds, not files on disk. Status is separate from an empty text. */
#ifndef LOCACHY_CLIPBOARD_H
#define LOCACHY_CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

#define CLIPBOARD_SOCKET "/run/locachy/clipboard.sock"
#define CLIPBOARD_VERSION 1
#define CLIPBOARD_TIMEOUT_MS 2500

enum { CLIPBOARD_REGULAR, CLIPBOARD_PRIMARY, CLIPBOARD_SELECTIONS };
enum { CLIPBOARD_GET, CLIPBOARD_SET, CLIPBOARD_CLEAR, CLIPBOARD_SYNC, CLIPBOARD_CHECK, CLIPBOARD_STATUS,
       CLIPBOARD_GET_PNG, CLIPBOARD_SET_PNG };
enum { CLIPBOARD_OK, CLIPBOARD_EMPTY, CLIPBOARD_NO_TEXT, CLIPBOARD_BUSY, CLIPBOARD_FAILED };

struct clipboard_message {
    uint32_t version, operation, selection, status;
    uint64_t generation, length;
};

/* All calls have a monotonic deadline; no Wayland dependency in the caller.
 * get: an owned fd on success, including zero bytes; status EMPTY/NO_TEXT
 * distinguishes no selection from a selection without text. */
int clipboard_get(int primary, int *fd, size_t *size);
int clipboard_set(int primary, const void *text, size_t size);
/* The same for an image (image/png) in the regular selection: a screenshot
 * to paste. get: status NO_TEXT when the selection has no image/png. */
int clipboard_get_png(int *fd, size_t *size);
int clipboard_set_png(const void *png, size_t size);
int clipboard_clear(int primary);
/* Wait for offers observed by the manager. This is not a global ordering
 * barrier for requests queued on other Wayland client connections. */
int clipboard_sync(uint64_t *generation);
int clipboard_check(uint64_t generation);
/* Compositor-gated freeze: begin -> capture -> freeze -> commit, bounded.
 * On failure this cancels the transaction and leaves the cgroup unfrozen. */
int clipboard_freeze(const char *cgroup);
int clipboard_request(struct clipboard_message *message, int *fd, int timeout_ms);
const char *clipboard_error(int status);

#endif
