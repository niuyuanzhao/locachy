# Running LOCachy Programs over SSH

[中文](SSH-TUI.zh.md)

The base programs that open a screen (`settings`, `packages`, `firmware`,
`files`, `archive`, `note`, `home`, and `view` and `edit` where they are built)
can draw inside a terminal session instead. An operator can maintain the machine
over SSH while the physical display is in use, for example by an extended
display (`extdisplay`). The backend is `base/tty-backend.c`. Process 1 (the
static musl init in the initramfs) is built without `CONFIG_TTY` and contains
none of it.

## When the terminal backend is used

- `LOCACHY_BACKEND=tty` forces it. If stdin or stdout is not a usable terminal,
  the program exits with status 69; a binary built without `CONFIG_TTY` exits
  with 78. It never falls back to DRM or input devices.
- With `LOCACHY_BACKEND` unset, it is chosen automatically when all of these hold:
  - the program has not called `screen_direct_only()`;
  - there is no process-1 control socket (`LOCACHY_CONTROL` unset);
  - stdin and stdout are terminals, and either `SSH_TTY` or `SSH_CONNECTION` is
    set, or stdin is a `/dev/pts/*` device.
- Any other explicit value (`direct`, `wayland`) keeps a program off this backend.
- `extdisplay` and `lock` call `screen_direct_only()`, which forbids automatic
  terminal selection so they stay on the physical DRM display unless
  `LOCACHY_BACKEND=tty` is forced.
- Selection and the in-band probe work through `sudo`.
  `/etc/sudoers.d/10-locachy-user` keeps `COLORTERM` and `LOCACHY_TTY_MODE`
  (without widening `LOCACHY_BACKEND`, which stays reserved for the trusted
  window launcher); `ttyname_r(0)` still resolves `/dev/pts/*` under `sudo`, and
  the in-band probe negotiates Kitty/Sixel/CSI u directly over the PTY.

## Rendering tiers

On open, the backend sends one in-band probe over the PTY: a Kitty graphics
query, a pixel-size query (`CSI 14 t`) when the window size has no pixel
dimensions, a Kitty keyboard query (`CSI ? u`), and a Primary Device Attributes
request (`CSI c`) as the end marker. Detection does not depend on `TERM_PROGRAM`.

1. **Kitty graphics** (Ghostty, Kitty, WezTerm, Konsole). The full software
   framebuffer of `screen.c` (XRGB8888) is shown pixel-exact: curves, icons,
   PDFs and FreeType CJK text. When the canvas is aligned to terminal rows (the
   normal case), only changed cell-row strips are sent; otherwise the whole frame
   is sent. Strips use ping-pong image IDs, zlib compression when built with
   `CONFIG_ZLIB`, and synchronized output (`DEC 2026`). HiDPI scale is the cell's
   pixel height divided by the font's text height (`TEXT_H`), in quarter steps
   from 1.0 to 3.0. Mouse: SGR pixel mode (`DEC 1016`).
2. **Sixel** (iTerm2, foot, Windows Terminal, xterm). Detected from attribute 4
   of the DA reply. The same framebuffer goes out as sixel bands aligned to whole
   terminal rows, with an adaptive palette (exact up to 256 colors, otherwise a
   6×6×6 cube plus 32 grey levels) and run-length encoding, inside `DEC 2026`.
   Mouse: SGR pixel mode.
3. **ANSI cell fallback** (macOS Terminal.app, tmux without graphics passthrough,
   plain SSH). A diff-rendered UTF-8 cell grid, with CJK widths matching
   `screen.c`, inside `DEC 2026`. Colors are 24-bit when `COLORTERM` is
   `truecolor` or `24bit`, or when `TERM` contains `kitty`, `ghostty`, `wezterm`,
   `foot`, `alacritty` or `direct`; otherwise the nearest xterm-256 color. Mouse:
   SGR cell mode (`DEC 1006`).

The managed sshd (`tailnet-ssh-client`) sets no `AcceptEnv`, so client variables
such as `COLORTERM` do not reach the session. `TERM` always does, because it
travels with the PTY request. Over that sshd, cell-mode TrueColor therefore
depends on `TERM`.

macOS's built-in Terminal.app implements neither Kitty graphics nor Sixel, not
even in macOS 26 Tahoe (which added 24-bit TrueColor), so it uses the cell
fallback automatically. Ghostty, Kitty, WezTerm and iTerm2 on macOS render the
full pixel framebuffer.

## Settings

| Variable | Effect |
|---|---|
| `LOCACHY_BACKEND` | `tty`, `direct` or `wayland`. Any other value exits with status 78 (see [WINDOW-BACKEND.md](WINDOW-BACKEND.md)). |
| `LOCACHY_TTY_MODE` | `auto` (default), `kitty`, `sixel` or `cell`; other values mean `auto`. A pixel mode falls back to `cell` if the terminal reports no pixel size or the pixel canvas cannot be set up. |
| `LOCACHY_NO_SUDO` | Set to any value, even an empty one, to stop `settings`, `packages` and `firmware` from re-running under `sudo` on a terminal. |

## Over SSH

- **Elevation:** `settings`, `packages` and `firmware` re-run themselves under
  `/usr/bin/sudo` when started as the LOCachy user over SSH or a PTY, so
  administrative changes need no explicit `sudo`. The password is the account's
  own, as with any `sudo`. An explicit `LOCACHY_BACKEND` turns that re-run off,
  because sudo drops the variable; the program then stays on the account it was
  started as.
- **Subcommands:** `settings` (`in_term()`) and `base_call()` hand the terminal to
  the command through `tty_backend_run()`: they leave the alternate screen,
  restore normal terminal mode, run the command on the same PTY, and repaint
  when it exits.
- **Input:** Kitty keyboard protocol (`CSI u`), xterm CSI and SS3 sequences,
  `modifyOtherKeys`, UTF-8 text, Ctrl/Alt/Shift modifiers, and a 40 ms wait that
  tells a lone Esc from an escape sequence. Mouse clicks, drags and wheel steps
  arrive as SGR pixel or cell events.
- **Clipboard:** `Ctrl+C` copies through OSC 52, up to 64 KiB of text. The
  terminal must allow OSC 52 writes.
- **Power:** `reboot` and `poweroff` elevate with `sudo` when needed, report if a
  `locachy-jobs` system job is running (init waits for it), and signal PID 1:
  `SIGTERM` for reboot, `SIGUSR2` for poweroff. `--force` does not wait and sends
  `SIGRTMIN+15` (reboot) or `SIGRTMIN+14` (poweroff). `home`'s reboot/power-off
  and `packages`'s `R` shortcut on the Updates page use the same path, except in a
  graphical window: there `R` cannot reach PID 1, says so, and points to home or
  SSH. An active `extdisplay` keeps running until the machine goes down.

## Tests

```sh
src/tools/test-tty-backend.sh [BUILD_CHECK_OUT_DIR]
```

`test-tty-backend.c` uses the `tty_backend_use_fds()` hook to verify in-band
probing (Kitty, Sixel, ANSI cell, typeahead), dirty-strip Kitty graphics with
ping-pong image IDs, Sixel exact/fallback palettes, TrueColor/256-color cell
diffs, keyboard/mouse decoding, OSC 52 clipboard, and `tty_backend_run()`.
When passed a `build-check.sh` output directory, `test-tty-backend.sh` also runs
the real binaries on a Linux PTY, answering the terminal probe itself:

- `settings` in ANSI cell, Kitty pixel framebuffer and Sixel pixel framebuffer modes;
- `settings` in Kitty mode on a terminal that reports no pixel size, which must fall back to cell mode;
- `packages` in cell and Kitty pixel framebuffer modes;
- `firmware` in cell mode.

Each run tests live `SIGWINCH` resizing and `Ctrl+Q` exit, and checks
`/proc/<pid>/fd` to confirm zero `/dev/dri/*` or `/dev/input/*` descriptors are
opened. The pixel runs need the Terminus fonts under `/usr/local/share/locachy/`.
On a host without them, the script copies the fonts from
`/usr/share/kbd/consolefonts` into a private mount namespace, which needs no root
privilege on the host.
