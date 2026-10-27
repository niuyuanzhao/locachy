/*
 * base/include/ime.h: Rime input method interface with Chinese & English completion.
 */
#ifndef BASE_IME_H
#define BASE_IME_H

#include <stddef.h>
#include <stdint.h>

#include "base.h"

#define IME_MODE_RAW 0 /* Direct passthrough, zero interception (default) */
#define IME_MODE_CN  1 /* Chinese pinyin */
#define IME_MODE_EN  2 /* English word completion */

#ifdef CONFIG_IME

/* Initialize Rime with shared_dir and user_dir. Returns 0 on success. */
int ime_init(const char *shared_dir, const char *user_dir);
void ime_done(void);

/* Focus control: 1 = text focus (IME active), 0 = complete passthrough. */
void ime_set_focus(int focused);
int ime_has_focus(void);

/* Mode control: IME_MODE_RAW, IME_MODE_CN, IME_MODE_EN.
 * ime_cycle_mode() rotates RAW -> CN -> EN -> RAW. */
int ime_cycle_mode(void);
void ime_set_mode(int mode);
int ime_mode(void);
const char *ime_mode_label(void);
const char *ime_mode_desc(void);

/* Chinese mode's punctuation, full width each time the mode starts: full
 * (the punctuation of Rime's table in default.custom.yaml only; other keys
 * as typed) or half (every key as typed), switched by Ctrl+. or Shift+Space
 * as in Chinese input methods; letters, digits and space are not changed. The
 * label shows it (mode-labels.conf). Returns the notice for the new width; NULL outside
 * Chinese mode. */
const char *ime_toggle_width(void);

/* rc.conf's keys for the input method, as keys.c gives them: key_ime cycles
 * the modes (K_CTRL_SPACE), key_ime_cn Chinese, key_ime_raw direct input,
 * key_ime_width Chinese mode's punctuation width. 1 when key was one of them
 * (done); *notice what to show, NULL for nothing. */
int ime_action(int key, const char **notice);

/* Process a key. Returns 1 if consumed by IME, 0 if passed through.
 * Control combos (Ctrl+C, Alt+x, etc.) always cancel preedit and pass through.
 * Either way the key may have committed text (ime_get_commit): type it before
 * passing the key on (a comma at half width ends the composition). */
int ime_process_key(int key);

/* Caps Lock, as Rime's front ends give it (ascii_composer with
 * good_old_caps_lock): its key ends the composition and turns ASCII mode on
 * when it locks, off when it unlocks; while it is locked, the keys pass by
 * Rime and the program types them (letters in capitals). ime_caps_key: the
 * key pressed (1) or released (0), locked as the keyboard had it before this
 * event (Linux's order); ime_set_caps: locked or not after it. */
void ime_caps_key(int pressed, int locked);
void ime_set_caps(int locked);

/* Returns committed text (if any) and clears it. Valid until next call. */
const char *ime_get_commit(void);

/* Check if composition is active and format the overlay lines (row1: 1..0,
 * row2: ^1..^0 when >10 candidates). Returns 1 if composing, 0 if idle. */
int ime_get_overlay(char *row1, char *row2, size_t size);

#else /* !CONFIG_IME */

static inline int ime_init(const char *s, const char *u) { (void)s; (void)u; return 0; }
static inline void ime_done(void) {}
static inline void ime_set_focus(int f) { (void)f; }
static inline int ime_has_focus(void) { return 0; }
static inline int ime_cycle_mode(void) { return 0; }
static inline void ime_set_mode(int m) { (void)m; }
static inline int ime_mode(void) { return 0; }
static inline const char *ime_mode_label(void) { return "RAW"; }
static inline const char *ime_mode_desc(void) { return " [RAW] Direct input "; }
static inline const char *ime_toggle_width(void) { return NULL; }
static inline int ime_process_key(int k) { (void)k; return 0; }
static inline void ime_caps_key(int p, int l) { (void)p; (void)l; }
static inline void ime_set_caps(int l) { (void)l; }
static inline const char *ime_get_commit(void) { return NULL; }
static inline int ime_get_overlay(char *r1, char *r2, size_t s) { (void)r1; (void)r2; (void)s; return 0; }
static inline int ime_action(int key, const char **notice)
{
    int ours = key == K_CTRL_SPACE || key == K_ACT_IME_CN || key == K_ACT_IME_RAW || key == K_ACT_IME_WIDTH;
    *notice = ours && key != K_ACT_IME_WIDTH ? ime_mode_desc() : NULL; /* direct input, as ime.c's says */
    return ours;
}

#endif /* CONFIG_IME */

#endif /* BASE_IME_H */
