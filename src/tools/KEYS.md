# Global keys and shared actions

[中文](KEYS.zh.md)

LOCachy splits keys in two.

- **Shared program actions** (`base/key-action.c`, documented in
  `KEYBOARDS.md` under *Keys and actions*) are the fixed keys every program
  reads through `key_action` / `key_list_action`: `Ctrl+Q` to quit, `Esc` to
  cancel (never quit), `q` as `Esc` in lists, `F1` for help, and the standard
  editing and navigation bindings. Programs react to the action, never raw keys
  (`test-key-actions`, `test-key-literals.py`).
- **Configurable global hotkeys** live in `/etc/locachy/rc.conf` as `key_*`
  entries. `base/keys.c` turns them into `K_ACT_*` codes for the direct
  framebuffer programs and process 1; `settings` writes them into labwc's
  `/run/locachy/labwc-rc.xml` (from `link/labwc-rc.xml`) for the shared Wayland
  display.

## Global search (`key_search`)

`key_search` opens LOCachy's global search (`/usr/local/bin/search`, see
`SEARCH.md`). Its default is `Super+Space`; make.sh adds `key_search="Super+Space"`
to an existing `/etc/locachy/rc.conf` only when the key is absent.

| Layer | Mechanism | Current status |
|---|---|---|
| Key table (`base/keys.c`, `base/include/base.h`) | `key_search` parses into `K_ACT_SEARCH`; key repeat is ignored, as for `K_ACT_HOME` and `K_ACT_LOCK`. | Wired. |
| Shared Wayland display (`link/labwc-rc.xml`, `progs/settings.c`, `progs/seat.c`, `init/apps.c`) | `SEARCH-KEY` binds to `/usr/local/bin/locachy-seat --window search`. `settings`' `labwc_line` replaces `SEARCH-KEY` with `key_search` (written to `/run/locachy/labwc-rc.xml`); `seat` accepts the window request `"search"`, and `init/apps.c` maps it to `/usr/local/bin/search` as a centered floating overlay (`LOCACHY_WINDOW_FLOATING=1`). A second press while it is open writes `close` to its request descriptor, so the key toggles it. | Wired for normal apply. `seat`'s fallback (`labwc_fallback_from`, used only when `settings --apply` fails) replaces `SEARCH-KEY` with its default `W-space` (Super+Space). |
| Host framebuffer (`init/init.c`, direct programs) | `K_ACT_SEARCH` opens `/usr/local/bin/search` on the focused framebuffer screen (process 1), without requiring the shared Wayland display; it is ignored while locked or while a shared display is held. Inside search, the same key closes it. | Wired in process 1. No other framebuffer program dispatches `K_ACT_SEARCH`. |
| Rebinding (`progs/settings.c`) | `settings` shows the key in a row, checks it against the other global keys (`key_taken`), writes it into labwc's `SEARCH-KEY`, and reloads labwc. | Wired: the row offers an edit (`choose_key`) and a delete. Change `key_search` in `/etc/locachy/rc.conf` and run `settings --apply` otherwise. |

## Other global hotkeys (`rc.conf`)

For the full list and rules for input method keys, Caps/Num Lock, and the shared
program action table, see `KEYBOARDS.md`.

- `key_home` (default `Super+Esc Ctrl+Alt+Esc Ctrl+Alt+Super ThinkVantage`):
  `K_ACT_HOME`, `HOME-KEY` in labwc; freezes the current task and returns to
  home.
- `key_lock` (default `Super+L`): `K_ACT_LOCK`, `LOCK-KEY` in labwc; locks the
  screen.
- `key_terminal` (default `Ctrl+Alt+T`): `TERMINAL-KEY` in labwc; opens a foot
  window on the shared display.
- `key_ime` (default `Ctrl+Space`), `key_ime_cn`, `key_ime_raw`,
  `key_ime_width`: input method keys (`IME-KEY` consumed in labwc; handled by
  `locachy-bar` on the shared display and `base/ime.c` on the framebuffer).
