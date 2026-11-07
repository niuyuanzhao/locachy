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

/* Initialize Rime with shared_dir and user_dir. Returns 0 on success. In a
 * window on the shared display (LOCACHY_BACKEND=wayland) no Rime is started:
 * the display's bar is the input method there, and the program stays in
 * direct input (0). */
int ime_init(const char *shared_dir, const char *user_dir);
void ime_done(void);
/* After the input method's packages may have changed (locachy-bar on SIGHUP):
 * Rime starts again, in the default directories, only when the Chinese schema
 * it would choose or its deployment is not the one it started with (Chinese
 * Pinyin installed or removed); 0 unchanged, 1 started again, -1 failed. */
int ime_reload(void);

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
 * as in Chinese input methods (locachy-bar: rc.conf's key_ime_width_cn);
 * letters, digits and space are not changed. The
 * label shows it (mode-labels.conf). Returns the notice for the new width; NULL outside
 * Chinese mode. */
const char *ime_toggle_width(void);

/* Whether key (as typed) with the modifiers held (keys_modifiers) is one of
 * rc.conf's key_ime_width_cn (Ctrl+. and Shift+Space unless set): the keys
 * that switch the width in Chinese mode only, and type as usual otherwise. */
int ime_width_key(int key, int mods);

/* rc.conf's keys for the input method, as keys.c gives them: key_ime cycles
 * the modes (K_CTRL_SPACE), key_ime_cn Chinese, key_ime_raw direct input,
 * key_ime_width Chinese mode's punctuation width. 1 when key was one of them
 * (done); *notice what to show, NULL for nothing. */
int ime_action(int key, const char **notice);

/* Process a key. Returns 1 if consumed by IME, 0 if passed through.
 * Chinese mode: digits choose from the candidates; control combos (Ctrl+C,
 * Alt+x, etc.) cancel the preedit and pass through. English mode, as fcitx5's
 * keyboard engine: letters make the word, Tab and Shift+Tab move through its
 * completions (none at first), BackSpace edits it; any other key (Space,
 * Return, digits, punctuation, Escape, arrows, combos) commits the letters as
 * typed, or the completion moved to, and passes through. Switching the mode
 * or locking Caps commits English mode's word too.
 * Either way the key may have committed text (ime_get_commit): type it before
 * passing the key on (a comma at half width ends the composition). */
int ime_process_key(int key);
/* Native XKB/X11 keysym and modifier mask, as Rime front ends supply them:
 * Shift, Lock, Control, Mod1, Mod4/Super; release is bit 30. Num Lock is
 * expressed by the resolved keypad keysym, not passed as a modifier. */
int ime_key_event(unsigned int sym, unsigned int mask);

/* Caps Lock is handled by the host front end, not by Rime's ascii_mode:
 * locking ends the composition and, while locked, keys bypass Rime and the
 * program types them (letters in capitals). ime_caps_key: the key pressed (1)
 * or released (0); ime_set_caps: locked or not after it. */
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
static inline int ime_reload(void) { return 0; }
static inline void ime_set_focus(int f) { (void)f; }
static inline int ime_has_focus(void) { return 0; }
static inline int ime_cycle_mode(void) { return 0; }
static inline void ime_set_mode(int m) { (void)m; }
static inline int ime_mode(void) { return 0; }
static inline const char *ime_mode_label(void) { return "RAW"; }
static inline const char *ime_mode_desc(void) { return " [RAW] Direct input "; }
static inline const char *ime_toggle_width(void) { return NULL; }
static inline int ime_width_key(int k, int m) { (void)k; (void)m; return 0; }
static inline int ime_process_key(int k) { (void)k; return 0; }
static inline int ime_key_event(unsigned int s, unsigned int m) { (void)s; (void)m; return 0; }
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
