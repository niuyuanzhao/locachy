# Private files dragged into a container

[中文](DOCUMENTS.zh.md)

Containers share the user's folders (`user_dirs` and Drives) at the same
paths, so a file there is dragged by its own path. Any other file of the
user's home is private to the host: no container sees it. Dragging one from
files into a container goes through the host's document service: the file
becomes a document of the packaged document portal (xdg-document-portal),
and only the container the drop lands on may read it, read only, until that
container stops.

The compositor alone knows where a drop lands, and only at the drop.
Receivers read a drag's data before it: Chromium on every enter, Firefox at
dragenter. So the documents exist, readable by nobody, from the start of
the drag, and labwc grants them to the target before it lets the drop
through.

## 1. Pieces

| Piece | Where, as whom | Lifecycle |
|---|---|---|
| `locachy-docs` (`progs/locachy-docs.c`), the service | host, root | `service_documents` of rc.conf (the desktop module's; make.sh adds it where `/usr/lib/xdg-document-portal` is): process 1 starts it, and starts it again 5 s after it ends. It runs in its own cgroup, `/sys/fs/cgroup/locachy-docs`, with everything it starts. |
| The documents bus (`dbus-daemon`, `link/docs-bus.conf`) | host, the user (uid 1000); socket `/run/locachy/docs/user/bus` | started by the service; only the user connects, and no container has the directory |
| `xdg-document-portal`, `xdg-permission-store` (package xdg-desktop-portal, unpatched) | host, the user, on the documents bus | started by the bus from their own service files; the portal mounts its FUSE file system at `/run/user/UID/doc` (fusermount3, `auto_unmount`, no `allow_other`); its data is in `/run/locachy/docs/user/data` (the documents are transient) |
| `locachy-docs --user`, the server | host, the user, no new privileges | started by the service with the control socket; a client of the portal on the documents bus |
| The control socket `/run/locachy/docs/control` | host only, root's (0600) | bound by the service, served by the server |
| A container's view | `/run/user/UID/doc` in the container | `/run/locachy/docs/views/NAME`, bound into the container by `link/locachy-app` |
| `container-documents` (`progs/container-documents.c`) | in each container, the user, on its own bus | owns `org.freedesktop.portal.Documents` there, as before, and answers `FileTransfer.RetrieveFiles` |
| files (`progs/files.c`, `base/wayland-data.c`) | host, root (a native window) | the drag's source |
| labwc's drop gate | host, root | a patch of `pkgbuilds/labwc-locachy` (not described here) |

The portal opens `/dev/fuse` as the user (libfuse has no fallback when that
fails), so the service first waits for the node as udevd makes it (0666, the
packaged rule): at boot it starts before udevd has described the devices,
and asks udevd for this one itself, as seat does for its own.

When the bus or the server ends, the service ends the rest (its cgroup),
detaches the views, and exits; process 1 starts it again, and it attaches
every view again. The portal leaving the bus ends the server.

## 2. A drag

1. **Begin.** An item drag leaves files' window. Files of the shared folders
   go by their paths, as before. For each other file of the home, files
   opens it as the user (`O_PATH`, without `O_NOFOLLOW`: what the user's
   path names) and sends the descriptors to the service, 16 a message
   (`begin`, `begin-more`). The server checks each one (an `O_PATH`
   descriptor that follows links, the file's own name, in the home, a file
   or a folder) and calls `AddFull(fds, flags, "", [])`: no application's
   permission, not persistent, not reused (each drag's documents are its
   own), `DIRECTORY` for folders. It answers the paths
   `/run/user/UID/doc/ID/NAME`.
2. **Offer.** Files offers `text/uri-list` with those paths (and the real
   paths of shared files) and `application/vnd.portal.filetransfer` and
   `application/vnd.portal.files` with a key of the same paths (section 4),
   copy only. A real path of a private file is never offered.
3. **Hover.** A container window under the pointer may read the list or the
   key. It learns the documents' paths and names; it can open none of them.
4. **Drop.** labwc holds the button release, asks the service
   `drop PID container NAME` (or `host`, `xwayland`), and lets the drop
   through only on `ok`. For a container, the server checks that it runs
   (its cgroup `app_NAME` is populated) and that its view is attached, then
   calls `GrantPermissions(ID, APPID, ["read"])` for each document.
5. **End.** files sends `end SESSION done` when the target read the drop,
   `end SESSION cancelled` otherwise, and closes the connection. labwc
   delivers the drop only on `ok` with the pointer still over the same
   client; when it gives up waiting (300 ms) or the pointer moved, it
   cancels, so a drag can end cancelled after its grants: they are revoked
   then.
6. **Revocation.** The server watches `cgroup.events` of each container it
   granted to; once the cgroup is empty (stopped, crashed), it deletes that
   container's documents. A frozen container keeps them.

## 3. The control socket

`/run/locachy/docs/control`, `SOCK_SEQPACKET`, one request a message; the
service binds it, root's alone (0600). The server takes a peer only as root
(`SO_PEERCRED`), and refuses even then a process of a container (cgroup
`app_NAME`; native windows, `app_window-*`, are root programs and may). It
checks with the peer's pidfd that the pid is still the peer's.

| Request | From | Answer |
|---|---|---|
| `begin\nNAME...` with one `O_PATH` fd per NAME, at most 16 | files | `ok SESSION\nPATH...` (one path a line, in order) or `error WHY` |
| `begin-more SESSION\nNAME...` with fds | files, same connection | the same; on an error every document of the session is deleted |
| `end SESSION done\|cancelled` | files, same connection | none: the connection's last message (files closes it); cancelled deletes the session's documents |
| `drop SOURCE_PID container NAME` | labwc (comm `labwc`) | `ok` after the grants, or `refuse WHY` |
| `drop SOURCE_PID host` | labwc | `ok`, nothing granted |
| `drop SOURCE_PID xwayland` | labwc | `refuse`: X11 programs cannot be told apart |
| `view NAME` | root of the host (not a window) | `ok` with an `O_PATH` fd of the portal's `by-app/APPID` |

labwc sends one request a connection and waits for its one answer. NAME is
the outermost `app_NAME` cgroup of the client under the pointer, with or
without a security context (so a container's xdg-desktop-portal-wlr on
`wayland-0` is that container). A native window (`app_window-PROGRAM-N`,
a root program process 1 runs on `wayland-0`, as files) is a host client,
as the drag's source and as its target: `drop SOURCE_PID host`, nothing
granted; `drop SOURCE_PID container window-...` is refused. No container is
named `window-...` (link/locachy-app refuses such names).

The session of a drop is the newest one begun by SOURCE_PID (the files
connection's peer) that has not ended or been dropped. A refused drop
deletes its documents at once (labwc then cancels the drag). A connection
that closes before its `end` cancels its sessions. Cancelling a session
that was granted to a container (`end cancelled`, the connection closing,
a grant failing, the container ending) first revokes each grant
(`RevokePermissions`), then deletes each document.

After `end done`: documents granted to a container stay until its cgroup is
empty; documents dropped on a host client stay until the portal exits
(transient; they are in no view); a drag never dropped has its documents
deleted.

The app id of container NAME is `org.locachy.app.` and NAME with `_` as `__`
and `.` as `_d`, after `_` when NAME begins with a digit
(`progs/docs-proto.h`): a valid app id for the portal, a different one for
each name.

## 4. The key and FileTransfer in a container

The key is `LOCACHYDOC` and the paths, NUL between them, in base64url
without padding: no `:` (Firefox takes data that parses as a URI for
something else), plain ASCII. files writes it with a terminating NUL, as
GTK 4 does.

`container-documents`, on each container's bus, adds
`org.freedesktop.portal.FileTransfer` (version 1). `RetrieveFiles(key, {})`
decodes the key itself and returns the paths (absolute, UTF-8), or
`InvalidArgument`; it grants nothing and asks nothing of the host: a key
made up in a container only names paths that container may or may not
read. `StopTransfer` succeeds; `StartTransfer` and `AddFiles` are
`NotAllowed`: a container is no drag source of documents. The Documents
interface stays as it was: `GetMountPoint` is `/run/user/UID/doc`, every
change is `NotAllowed`.

A host program (`host="1"`) gets `container-documents` too: a document
portal of its own would mount over its view.

## 5. Each container's view

The portal's mount shows a container only `by-app/APPID`: the documents
that app id may read. Its FUSE mount is the user's without `allow_other`,
so root cannot look a path up in it. The service therefore:

1. keeps `/run/locachy/docs/views` a tmpfs of its own, shared, and makes
   `views/NAME` in it (`locachy-docs --attach NAME` from `link/locachy-app`,
   before the container enters its cgroup, or the service for every
   `views/NAME` when it starts);
2. asks the server (`view NAME`) for an `O_PATH` fd of `by-app/APPID`,
   opened as the user;
3. clones that fd's tree (`open_tree(OPEN_TREE_CLONE)`), makes the clone
   private, and attaches it on `views/NAME` (`move_mount`), in place of
   what was there.

`link/locachy-app` binds the directory `views/NAME` itself at
`/run/user/UID/doc` in the container, as a slave of the views mount
(`docs_bind`), never the view attached on it then: a copy of that would
stay, dead, once the service started again. It finds the directory in a
bind of the views mount alone, under the service's lock, and decides from
the mount table (`docs_select`): root cannot stat an attached view. Once
the start is over, the view is attached again (`docs_reattach`, forked
before the launcher enters `app_NAME`), and that attach reaches the
container; so does every later detach and attach (the service started
again). `app_NAME` is made 0755, as process 1 makes its windows': the
server, the user's, reads its `cgroup.events`. A host program gets the same view in its own runtime directory, and
`/run/locachy/docs` hidden under an empty tmpfs: any peer of the documents
bus may grant any document.

Without a view (the service not running, the portal not installed) the
container's `/run/user/UID/doc` is an empty directory, and a drop on it is
refused.

## 6. Failures

| Case | Result |
|---|---|
| The service not running when the drag starts | No drag; files says the document service is not running. |
| The portal refuses a file (`AddFull` fails), a renamed file, a FIFO, a file outside the home | No drag; nothing of it stays (all or nothing). |
| Drop on Xwayland, on a container without a view or not running, a grant that fails | `refuse`; the documents are deleted; labwc cancels the drag. |
| labwc gets no answer in time (300 ms), or the pointer left the target | labwc cancels the drag; files sends `end cancelled`; grants already made are revoked and the documents deleted. |
| Escape, a drop on nothing, a target that refuses | wlroots cancels; `end cancelled`; deleted. |
| files ends during the drag | its connection closes; deleted. |
| The container stops | its documents are deleted. |
| The portal or the bus ends | the service ends and starts again; documents are gone (transient); views attached again. |

## 7. Isolation, power, speed, security

- **Isolation.** A container sees only its own `by-app` view, and in it only
  what was dropped on it. No host bus is in a container, and the host
  parses nothing of a container's bus. The service, the bus, the portal and
  the server are one managed cgroup, ended together.
- **Power.** Everything waits on events: the control socket, D-Bus, FUSE,
  `cgroup.events` (inotify). Nothing polls.
- **Speed.** One round trip to the portal per 16 files at drag start; at the
  drop, one `GrantPermissions` per document before labwc lets it through.
- **Security.** New surface: the portal's FUSE server, reachable from every
  container through its own view (as in Flatpak); the root-only control
  socket; the documents bus, the user's, never in a container. Grants are
  read only and transient.

## 8. Checks

- `tools/test-docs.sh NEW_DIR`: `progs/docs-proto.h` (the key both ways,
  every byte; malformed keys refused; app ids and their escapes; peers told
  apart), then the server (normal and ASan/UBSan) against a stand-in portal
  on a private bus, as root of a user namespace: a peer not root shut out,
  drops only from comm `labwc`, `AddFull` without app or permission and
  `DIRECTORY` for folders, `end cancelled`, a closed connection, a drop on
  the host, all or nothing over `begin-more`, descriptors refused (renamed,
  outside the home, `O_NOFOLLOW`, not `O_PATH`, a FIFO), drops refused
  (Xwayland, no view, not running, a second one, a native window named as
  a container), `end` unanswered, the grant for the
  container's app id, documents kept while it runs or is frozen and deleted
  once its cgroup is empty, granted and then cancelled (or its connection
  closed) revoked and deleted, `view NAME`.
- `tools/test-docs-portal.sh NEW_DIR`: the service itself with the packaged
  portal and permission store, in a user, mount and PID namespace (the
  user's processes keep `CAP_SYS_ADMIN` of the namespace, as fusermount3 is
  not setuid there): the control socket root's alone; root unable to look
  into the user's FUSE mount; a view attached through `view` and
  `open_tree`/`move_mount` reaching a "container" mount namespace started
  before; the view selection of link/locachy-app itself (`docs_select`,
  sourced from it) naming an attached view that root cannot stat; a
  container started while the service runs (`docs_bind`, `docs_reattach`)
  reaching the view attached after its start; a file and a folder
  unreadable there until the drop, readable after it, read only, not in
  another container's view; deleted when the cgroup empties; the service
  ending everything and, started again, attaching the views again, into
  both running containers; granted and then cancelled, no longer readable.
  Namespace root only: the real root is `tools/test-docs-view.sh`.
- `tools/test-docs-view.sh [NAME]`: VM only, as the real root of a running
  system: the view `locachy-docs --attach` attached for NAME (the user's FUSE
  mount) closed to root's stat, and `docs_select` of the installed
  locachy-app naming it all the same.
- `tools/test-files.sh`: files' side (see [WAYLAND-DATA.md](WAYLAND-DATA.md)).
- `tools/test-container-documents.sh`: `RetrieveFiles` with a key made by
  coreutils' base64, a bad key, `StartTransfer`, `AddFiles`, `StopTransfer`.
- `tools/test-drop-gate.sh` (labwc, in the VM's gate): a native window as
  the drag's source and as its target, and a window named as a container.

Not covered by these: the service as the real root of LOCachy (its cgroup,
`/sys/fs/cgroup/app_NAME`, `link/locachy-app` starting a real container)
except by `test-docs-view.sh`, and a browser reading a dropped document.
