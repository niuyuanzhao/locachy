/* SPDX-License-Identifier: GPL-2.0-only
 * LOCachy's clipboard freeze handover. Selection handlers call allowed()
 * synchronously while dispatching the client's protocol request.
 * client_cgroup() is the pidfd-checked cgroup directory (O_PATH, owned
 * here) of a client's creator, or -1. */
#ifndef LABWC_LOCACHY_CLIPBOARD_H
#define LABWC_LOCACHY_CLIPBOARD_H
#include <stdbool.h>
struct wl_client;
void locachy_clipboard_init(void);
void locachy_clipboard_finish(void);
bool locachy_clipboard_allowed(bool primary);
int locachy_client_cgroup(struct wl_client *client);
#endif
