/* SPDX-License-Identifier: GPL-2.0-only
 * LOCachy's clipboard freeze handover. Selection handlers call allowed()
 * synchronously while dispatching the client's protocol request. */
#ifndef LABWC_LOCACHY_CLIPBOARD_H
#define LABWC_LOCACHY_CLIPBOARD_H
#include <stdbool.h>
void locachy_clipboard_init(void);
void locachy_clipboard_finish(void);
bool locachy_clipboard_allowed(bool primary);
#endif
