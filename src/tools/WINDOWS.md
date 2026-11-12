# Host programs as managed windows

[中文](WINDOWS.zh.md)

view, files, settings, mixer, bluetooth, network and term can be windows of
their own on the shared display, besides running on the direct screens from
home. A window is a managed instance of the same program, not a copy of it:
process 1 starts, freezes and stops it like a container, it never holds a
screen lease or an input device, and it keeps the shared display running while
it is open.

## Requests

`locachy-seat --window PROGRAM [TARGET]` asks for one, and `locachy-seat --open
[--own-tor] CONTAINER` asks process 1 to open or raise a container on the
shared display. The top bar runs them (its launcher mirrors home's sections,
its Bluetooth block opens bluetui when the service is on, its volume block the
mixer, and its other right-hand status blocks open settings at a page), and so
does the bar's xdg-open for a container on the shared display: a document view
opens goes to a view window, a folder to a files window, instead of switching
to home.

| PROGRAM   | TARGET                                              | Command run |
|-----------|-----------------------------------------------------|-------------|
| settings  | none, or `network`, `power`, `devices`, `date-time`, `hardware` | `/usr/local/bin/settings [--page=PAGE]` |
| view      | none (its chooser), or an absolute path of a file   | `/usr/local/bin/view [FILE]` |
| files     | none (home), or an absolute path of a folder        | `/usr/local/bin/files [FOLDER]` |
| mixer     | none                                                | `/usr/local/bin/term /usr/bin/wiremix` |
| bluetooth | none                                                | `/usr/local/bin/term /usr/bin/bluetui` |
| network   | none                                                | `/usr/local/bin/term /usr/bin/nmtui` |
| term      | none                                                | `/usr/local/bin/term /usr/local/lib/locachy/user-shell` |

The terminal opens the configured user shell as UID 1000, not an administration
shell. The settings network page also runs nmtui in a child terminal window in
settings' managed cgroup and remains responsive while waiting for it to close.

The launcher uses home's shared host-program classification and all registered
programs, not a fixed short list. Enter or left-click opens a supported window;
F or right-click opens its full-screen entry through home. Direct-only programs
use home. Reboot and power off open home's existing confirmation prompt; they
never execute directly from the launcher. Container state here is a cgroup/ready
marker snapshot, not home's complete process-1 state, and containers are sorted
by name. RAM, swap, CPU and temperature blocks are readings, not buttons.

Nothing else is accepted: no other program, no arguments of the caller's, no
arbitrary shell command. The request travels on existing channels only. The
client sends it to labwc-control's socket (`/run/user/0/locachy-control.sock`, root only);
labwc-control passes its fields to the seat on a socket pair the seat gave it
(`LOCACHY_SEAT_REQUESTS`); the seat sends `window PROGRAM [TARGET]` on its
control socket to process 1. The client, the seat and process 1 each check the
program and target; process 1 also checks that view's file exists. While the
screen is locked process 1 refuses window requests.

## Lifecycle

Process 1 (`init/apps.c`, `apps_window_open`) keeps windows in memory only,
next to the containers: there is no configuration file and no recipe. Each
window is named `window-PROGRAM-N`, which is also its cgroup (`app_NAME`), its
log (`/run/locachy/apps/NAME.log`) and its Wayland app id. The program runs as
root, as on the direct screens, with:

- `LOCACHY_BACKEND=wayland`, `XDG_RUNTIME_DIR=/run/user/0`, `WAYLAND_DISPLAY=wayland-0`
- `LOCACHY_WINDOW_TITLE`, `LOCACHY_WINDOW_APP_ID`
- `LOCACHY_WINDOW_READY_FD=3`: it writes one byte once its first window is
  configured and drawn. Until then the window is `starting`; a program that
  ends or closes the descriptor first has failed (home shows why).
- `LOCACHY_REQUEST_FD=4`: requests while it runs (`page NAME` for settings).
- no `LOCACHY_CONTROL`: it never borrows process 1's control socket.

A window waits for the shared display's display-ready, as a container does
(the first window after boot), and counts towards the display's state: the
display starts for it and is not stopped while a window runs, even when no
container does.

settings, the mixer, bluetooth and network have one window each: another
request raises it, and a settings page goes to the open window as `page NAME`
instead of a second copy. view, files and term open a window per request (at
most sixteen windows).

Closing the window (its close button, Alt+F4) reaches the program as
`K_CLOSE`, and the program leaves through its own quit path:

- files leaves as `q` does, also from a question or a list (the question is
  answered no). During a copy it asks first: `y` stops the copy now, `n`
  closes once the copy is done, Esc keeps the window.
- view leaves as Ctrl+Q does; with unsaved changes it asks to save or
  discard them first, and the window stays open meanwhile.
- settings leaves as `q` does, also while one of its package, repositories
  or boot image jobs runs or waits: those are system jobs (`locachy-jobs`,
  [JOBS.md](JOBS.md)), run apart from the window, which go on without it;
  home lists them, and settings, opened again, follows them again.
- the mixer (`term`) hangs up its program's process group (SIGHUP), as a
  terminal does when it closes, and ends.

When the program has ended, process 1 ends whatever it left in its cgroup
(`cgroup.kill`) and forgets it. Home stops, freezes and resumes a window as
a container: a freeze goes through the clipboard service, which gates the
cgroup as for a container; a stop thaws, saves the clipboard, sends SIGTERM
to the cgroup's processes and finally `cgroup.kill`. Home's Stop stays
forceful; it reaches no system job a window asked for, and a frozen window
freezes none either: those run in cgroups of their own under locachy-jobs.
files is the exception that cleans up:
on SIGTERM during a copy it removes the unfinished copy first (below).

## Opening files from a window

A window has no process 1 to ask, so files opens what it is asked to
differently from home's file manager, and says when it cannot:

- a file whose program is the document viewer as installed
  (`/usr/local/bin/view`) opens in a view window of its own, through
  `locachy-seat --window view PATH`, run by itself as the top bar runs it;
- a file for a container goes to the top bar's open socket
  (`/run/locachy/open/socket`, `with NAME TARGET`, xdg-open's protocol). The
  bar and files both keep a container to the shared folders;
- a folder is browsed in the window;
- any other framebuffer program (edit, archive, term) is refused with a
  message that it opens from home only;
- the requests left for home (`/run/locachy/files-request`, `open-request`)
  stay home's.

## Copies

files writes each copied item under a hidden name in the destination folder
(`.locachy-copy-PID-N`) and renames it only once it is whole, so a partial
file never has the final name. A move across filesystems is such a copy,
and its source is removed only after the rename. An item that fails, is
stopped (Esc asks first) or is cut short by SIGTERM is removed; items already
done stay and can be undone. Between chunks files keeps answering: it draws
the progress, takes a new size, and reads Esc and the close. Only a SIGKILL
(`cgroup.kill` after the 3 s home's Stop gives) or a power loss can leave a
hidden `.locachy-copy-*` behind.

A drive without the atomic no-replace rename (`renameat2` with
`RENAME_NOREPLACE` gives EINVAL, as an rclone cloud drive does) is found by
renaming an empty hidden file of files' own there, which is then removed.
There each item is written under its own name instead, created exclusively
(`O_EXCL`, `mkdir`), so a name created meanwhile is still never replaced.
An unfinished file is removed before it is closed: rclone uploads a file
when it is closed and never one removed while open, so a cloud gets only
whole files. Moves within such a drive are refused ("cannot move within
itself without overwrite protection"): as a copy and a removal they would
download and upload everything again.

## Not yet

- Raising an open window relies on labwc-control matching a window's app id to
  its `window-PROGRAM-N` name.
- The programs need the Wayland backend (`LOCACHY_BACKEND=wayland`); without
  it a window fails visibly instead of taking a screen.

## Checks

- `tools/test-app-registry.sh OUT`: the registry with stand-ins for the
  programs and the clipboard: refused requests, the environment and
  descriptors given, readiness, display state, one settings window and its
  page requests, closing, freeze/resume/stop, failures, display-ready.
- `tools/test-window-routing.sh OUT`: `locachy-seat --window` against a
  stand-in socket, the seat's forwarding and labwc-control's part.
- `tools/test-files.sh OUT`: among the file manager's scenarios, copies under
  a hidden name (Esc, the close, a failure, SIGTERM, moves across
  filesystems) and opening from a window against stand-ins for the seat
  client and the top bar's open socket.
- `tools/test-settings-close.c` and `tools/test-term-close.c` (in
  `test-interaction-regressions.sh`): settings' close with and without a
  system job, which it leaves to the queue and follows again when reopened;
  term hanging up its program's process group when closed.
