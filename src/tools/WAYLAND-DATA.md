# Clipboard and drags of programs in Wayland windows

[中文](WAYLAND-DATA.zh.md)

A direct program drawn in a Wayland window (`LOCACHY_BACKEND=wayland`) uses
that window's standard data device (`base/wayland-data.c`): `wl_data_device`
for the clipboard and drags, `zwp_primary_selection_device_v1` for the primary
selection. No compositor-private protocol and no host clipboard socket. The
same program on a direct screen keeps the host clipboard service.

## Selections

- `clipboard_set`/`clipboard_set_png` offer the text (as
  `text/plain;charset=utf-8`, `UTF8_STRING`, `text/plain`) or `image/png`,
  with the window's latest keyboard or pointer serial.
- `clipboard_get` returns a sealed memfd with the exact size, as the host
  service does. Another client's selection is read from its pipe with the
  host service's limits: 2.5 s (then BUSY) and 64 MiB. The program's own
  selection is answered from memory: asking the compositor for it would wait
  for the same event loop.
- Requests from other clients are written without blocking: at most 16 at
  once, each ending at a 3 s deadline, end of file, or the reader's close.
  A reader that closes first causes no SIGPIPE.

## Drags to other programs

In files, an item drag that leaves the window becomes a standard drag:
`text/uri-list` of percent-encoded `file://` URIs, started with the real
press serial, copy only (the files never move or disappear). The window is
not a drop target yet: a drag over it is refused.

A file whose real path (symbolic links resolved) is in the shared folders,
`user_dirs` (Documents, Downloads, Music, Pictures and Videos by default) and
Drives, goes as its own path: isolated programs see those folders at the same
paths.

Any other file of the user's home goes as a document of the host's document
service ([DOCUMENTS.md](DOCUMENTS.md)). Before the drag starts, files opens
each one as the user (`O_PATH`, following links as the user's path does) and
sends it to the service, which makes it a document that no program may read
yet. The drag then offers, built once:

- `text/uri-list`: the documents' paths (`/run/user/UID/doc/ID/NAME`) for
  those files, the real paths for shared ones; a real path of a private file
  is never offered;
- `application/vnd.portal.filetransfer` and `application/vnd.portal.files`:
  a key of the same paths, which a container's `RetrieveFiles` decodes.

Only the program the drop lands on may read them: the compositor asks the
service before it lets the drop through. When the drag ends, files tells
the service (`end SESSION done` once a target read the drop, `cancelled`
otherwise), and closes its connection. Such a drag needs data device
version 3, which tells its end.

All or nothing: if the service does not run, or refuses one file, the drag
does not start and a message says why. A file outside the home (a link to
`/etc`, a folder browsed outside it) is refused.

## Checks

- `tools/test-wayland-data.sh NEW_DIR` (as root in the development root):
  write-queue unit checks, normal and ASan/UBSan, with a drag with a key
  (each type's bytes, the end told once); then a real headless labwc
  window with a virtual keyboard and pointer: selections both ways with a
  data-control client, primary, `image/png`, a silent source ending as BUSY,
  clear, a drag dropped on foot (which reads the URI), and a drop on the
  window itself cancelled. Built without `CONFIG_WAYLAND`, the file needs no
  Wayland library.
- `tools/test-files.sh`: an item dragged past the window edge stays internal
  on a direct screen; in a window, a shared file starts one drag with the press
  serial and the encoded URI; a link out of the home is refused; a private file
  is refused without the document service, and when the service refuses it;
  with a stand-in service, its O_PATH descriptor is sent, its document's path
  (never its own) is in the URI list and the key, and the drag's end
  (done, cancelled) reaches the service.

A real browser receiving a dropped file is acceptance outside these checks.
