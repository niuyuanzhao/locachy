/* Opt-in non-text keyboard events. Existing key_get consumers are unchanged. */
#ifndef LOCACHY_INPUT_RAW_H
#define LOCACHY_INPUT_RAW_H
#include <stdint.h>
#define K_RAW_KEY 0x1ED
#define BASE_RAW_CODES 768u
#define BASE_RAW_DEVICES 32u
#define BASE_RAW_LOCAL 1u
#define BASE_RAW_WAYLAND_DEVICE 0x80000001u
struct base_raw_key {
    uint32_t device; /* session identity: evdev st_rdev + 1, or the Wayland seat */
    uint32_t code;   /* Linux evdev physical code, not an XKB keycode */
    uint32_t keysym; /* XKB keysym at press, retained through repeat and release */
    uint32_t modifiers; /* KEYS_SHIFT/CTRL/ALT/META/ALTGR/CAPS plus KEYS_NUM=64 */
    uint32_t locks;  /* 1 Caps Lock, 2 Num Lock */
    uint32_t state;  /* 0 release, 1 press, 2 repeat, 3 cancel-all */
    uint32_t flags;  /* BASE_RAW_LOCAL: binding belongs to the local system */
};
/* Enabling/disabling flushes old events and held mappings. Enable before
 * polling, drain with base_raw_next on K_RAW_KEY (also after every ordinary key).
 * Cancellation invalidates every held key; never infer missing releases.
 * Direct device loss/SYN_DROPPED, Wayland leave/keymap loss, queue overflow and
 * input revocation emit a cancel. Wayland reports the seat, not physical IDs. */
int base_raw_enable(int on);
int base_raw_next(struct base_raw_key *out);
/* Fractional scroll samples in pointer_last(), independently opt-in from the
 * existing pointer_scroll_events boundary API. Returns the previous setting. */
int pointer_scroll_precision(int on);
/* Backend hooks are weak so existing standalone keyboard tests need not link
 * this optional consumer. Programs use the strong API above. */
__attribute__((weak)) int base_raw_pending(void);
__attribute__((weak)) void base_raw_emit(uint32_t device, uint32_t code, uint32_t keysym,
                                        uint32_t mods, uint32_t locks, uint32_t state, int local);
__attribute__((weak)) void base_raw_cancel(void);
#endif
