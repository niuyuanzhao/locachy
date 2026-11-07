# Direct programs as windows

[中文](WINDOW-BACKEND.zh.md)

The base's programs (term, settings, view, files) draw into a software canvas.
On a screen of their own they show it through DRM, with input devices lent by
process 1. The same programs can instead show it as an ordinary window on the
shared display. Only the trusted window launcher chooses this, by setting
`LOCACHY_BACKEND=wayland`; anything else, including an unset variable, is the
direct path, unchanged.

## The choice is explicit and fails closed

- `direct` (or unset): screens and devices exactly as before.
- `wayland`: an xdg-shell window. No DRM node, input device, lease or
  process-1 control channel is used.
- A program built without the window transport, or any other value of the
  variable, ends with status 78. A window request with no compositor (or no
  window from it) ends with status 69. Neither falls back to opening DRM or the
  input devices.

Process 1 and the initramfs are built with musl and contain no window code.
`make.sh` builds the window transport (`CONFIG_WAYLAND`) only into the
programs that open windows, and only when the desktop module's Wayland
libraries are installed.

## What the window does

- **Frame:** the window asks the compositor for its own frame
  (`zxdg_decoration_manager_v1`, server side): a title bar with the title and
  the close, maximize and minimize buttons. labwc draws none for a window that
  does not ask.
- **No top bar of its own:** the desktop's bar shows the status, so a window
  has no LOCachy top bar. The canvas keeps the bar's rows above the window, so
  a program lays out exactly as on a screen, with its first content row at the
  window's top edge. `screen_bar` draws nothing in a window and those rows are
  never shown. Pointer positions are in canvas pixels; a point above the window
  (while a button is held) is negative, off the canvas.
- **Picture:** the canvas has the window's size in pixels at the preferred
  fractional scale (`wp_fractional_scale_v1` with `wp_viewporter`; an integer
  buffer scale otherwise). Each new size, the first configure included, comes
  to the program as `K_SHOWN`, as a new screen does, and the program lays out
  again. A picture is shown in `wl_shm` buffers, with at most one frame in
  flight; a picture drawn while the compositor still shows the last one waits
  for that frame. When the compositor leaves the size to the program, the
  window starts at 1100x760 logical pixels, or smaller to fit the bounds the
  compositor recommends.
- **Keys:** the seat's keyboard, with the compositor's keymap (and the input
  method's, which delivers text the same way), XKB compose sequences and key
  repeat. Keys become the base's codes as on a screen: arrows and function keys,
  Ctrl+letters, Ctrl+digits, and `K_TEXT` for text beyond ASCII. Global
  shortcuts (home, lock, input method, volume) are the compositor's and never
  reach a window.
- **Pointer:** motion, buttons, double and triple clicks, wheel steps, and
  touchpad scrolling in the same steps as on a screen. The compositor draws the
  cursor.
- **Readiness:** the first picture shown writes one byte to the launcher's
  `LOCACHY_WINDOW_READY_FD`, then closes it. Children never inherit it.
- **Closing:** the close button (or the compositor's close binding) does not
  end the program. It comes to the program as the key `K_CLOSE` (`base.h`),
  and the program leaves through its own quit, so work in progress (a copy, a
  package installation) is not cut off. Closing again while the program has
  not yet read `K_CLOSE` adds nothing.
  - A program that stays open on purpose (work in progress, unsaved changes)
    calls `key_close_kept()` after reading `K_CLOSE`. The next close is a new
    `K_CLOSE`.
  - The safety net: a second close within 3 s of the program reading `K_CLOSE`,
    with no `key_close_kept()` in between, ends the program at once (status 0).
    This is for programs that do not know `K_CLOSE` yet. After 3 s, a close is
    a new `K_CLOSE` again.
  - When the compositor goes away, the window cannot be shown any more and the
    program ends with status 69.

Selections, the clipboard and dragging files out of a window are the data
device's (`base/wayland-data.c`), which uses this window's connection, seat,
surface and input serials.

## Checks

`tools/test-window-backend.sh NEW_ABSOLUTE_DIR` runs in a disposable test root
that has labwc. It starts a private headless labwc with no physical input or
display, then checks:

- the three refusals (statuses 78, 78 and 69);
- the readiness byte and the first configure;
- that no DRM or input device is open;
- the picture on the output (screenshot), without the program's top bar;
- the server-side frame negotiated, with the window below its title bar;
- keys and a click from virtual devices;
- a maximize resize;
- closing with Alt+F4:
  - `K_CLOSE` arrives and the program keeps running;
  - `key_close_kept()` makes a quick second close another `K_CLOSE`;
  - a close more than 3 s later is another `K_CLOSE`;
  - a second close within 3 s ends the program.

It does not cover the following:

- the frame's close button clicked with the pointer (Alt+F4 sends the same
  request);
- a fractional output scale;
- touch input;
- a running input method;
- the window programs' own layouts (settings, files, view);
- real hardware.
