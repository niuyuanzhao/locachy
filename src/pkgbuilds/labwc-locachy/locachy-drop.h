/* SPDX-License-Identifier: GPL-2.0-only
 * LOCachy's drop gate. The input handlers call these instead of the seat's
 * own button and touch-up notifications; everything else is unchanged.
 * escape() is asked about a key press first: true when it cancelled a drag
 * and consumed the key. */
#ifndef LABWC_LOCACHY_DROP_H
#define LABWC_LOCACHY_DROP_H
#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>
void locachy_drop_pointer_button(struct wlr_seat *seat, uint32_t time_msec,
    uint32_t button, enum wl_pointer_button_state state);
void locachy_drop_touch_up(struct wlr_seat *seat, uint32_t time_msec, int32_t touch_id);
bool locachy_drop_escape(struct wlr_seat *seat, const xkb_keysym_t *syms, int count);
void locachy_drop_finish(void);
#endif
