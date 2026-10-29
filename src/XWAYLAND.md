# Xwayland

The desktop module uses labwc's built-in Xwayland. There is one shared labwc
and one shared X server, never a compositor or an X server per application.
`xorg-xwayland` and `xorg-xhost` are desktop module packages. Programs that
speak Wayland keep using Wayland.

## Session

`/etc/locachy/labwc/rc.xml` sets
`<core><xwaylandPersistence>yes</xwaylandPersistence></core>` (labwc-config(5)): Xwayland starts with the shared display and runs as long as
it does. labwc runs `/etc/locachy/labwc/xinitrc` (labwc-config(5)) each time it
starts Xwayland, with `DISPLAY` set to the display it chose (not assumed to be
`:0`). That script:

1. makes the display's filesystem socket connectable for everyone (0777), as
   an X server's own sockets are. wlroots creates it with labwc's umask (0755),
   which lets only root connect; containers and host programs have network
   namespaces of their own and can use only this socket, not the abstract one;
2. adds the application user (uid 1000) to Xwayland's access control list,
   `xhost +si:localuser:USER` (xhost(1)). Xwayland itself admits only the user
   it runs as, root. No host or network entry is added and access control is
   never disabled: the list, not the socket mode, decides who may connect;
3. then writes the display to the root-only runtime file
   `/run/user/0/locachy-x11-display`. An empty file means X11 is unavailable;
   Wayland is not affected.

The access list belongs to the running X server. Persistence keeps it for the
whole session; if Xwayland ends anyway (a crash), labwc starts it again on the
same display and runs xinitrc again.

Measured alternatives (Xwayland 24.1.14, labwc 0.20.2):

- labwc's default, a lazy Xwayland, starts at the first X11 client and ends
  10 s after the last. The client that starts it is refused, at the first
  start, after an idle end and after a crash alike: it connects before xinitrc
  has added the user. Later clients are admitted.
- `/etc/X0.hosts` (Xserver(1)) is read: a `local:` line appears in the list,
  yet uid 1000 is still refused. A `si:localuser:USER` line does not enter the
  list at all (a server-interpreted value is "type NUL value", os/access.c
  siCheckAddr, which a line of text cannot hold). The file also names a fixed
  display number, while labwc chooses its own.

`locachy-seat` starts labwc without `DISPLAY` or `XAUTHORITY`, removes the
record at the start of each session and at its end, and tells process 1
`display-ready` once the record is there (when Xwayland is installed; after
10 s it goes on without X11 and says so: this fallback is not measured).
Xwayland is labwc's child, in the shared display's cgroup, not in an
application's.

## Containers

`locachy-app` reads the record and validates it (`:N`) without evaluating it as
shell code; if the display's socket is not there (Xwayland restarting), the
program starts without X11 and says so. Only a program whose configuration
says `x11="1"` gets X11 (see the security boundary below); any other stays
Wayland only, with no `DISPLAY` and no X11 socket. Such a container gets
`DISPLAY` and a read-only bind mount of that display's socket in its private
`/tmp/.X11-unix`; nothing else of the host `/tmp`, no other display, no TCP
access, no `XAUTHORITY`. A host program (`host="1"`) with `x11="1"` gets the
same `DISPLAY`. Applications keep their own managed process trees and their
freeze and stop controls.

## Security boundary

X11 clients connected to the same X server are **not isolated from one
another**: a client that can connect "has full run of the screen"
(Xserver(1)), including other X11 windows, their input and the X11
selections, which restricted Wayland clients cannot reach. Container
filesystem isolation does not change this. Programs that support Wayland
should use it; the restricted Wayland socket `locachy-0` is unchanged.

## Clipboard

The host clipboard (`clipboard`) reads each new selection at once and offers it
again as its own. Xwayland's selection bridge (wlroots' xwm) turns an X11 copy
into a Wayland selection, and takes the X11 selection over for the host
clipboard's offer. Measured:

- an X11 copy in a container reaches the host clipboard and other X11
  clients; the container then no longer owns it;
- frozen with `locachy-app freeze` (it cannot answer), or stopped, the
  container's copy stays in the host clipboard and X11 clients paste it;
- a `locachy-app freeze` started while the host clipboard is still reading a
  slow X11 copy waits for the transfer (its sync), then freezes.

**Gap.** The freeze barrier of labwc-locachy (the cgroup gate between
`clipboard freeze`'s capture and its freezer write) covers Wayland clients
only. xwm forwards an X11 copy from its own X connection (the clipboard with
`wlr_seat_request_set_selection` and no Wayland client, the primary selection
with `wlr_seat_set_primary_selection` directly), so the gate cannot attribute
it to the container's cgroup. Measured, with the window widened by the test:

- an X11 copy made by the container between the freeze's sync and its commit
  is accepted. The guard test (`tools/test-clipboard-guard.sh`) measures that
  a Wayland copy by a gated cgroup is refused while its gate is open;
- if the commit freezes the container while the host clipboard is still
  reading that copy, the read cannot complete: right after the freeze the host
  clipboard reports "transfer is not complete", and after the resume (2.5 s
  later) "transfer failed". X11 pastes get nothing while frozen, and the
  copy again after the resume. The host clipboard takes the next X11 copy
  normally.

In use the window is only the time between one freeze's sync and its freezer
write (its length was not measured), and the container has to copy within it.
Closing it needs a change to the labwc-locachy guard
(`pkgbuilds/labwc-locachy`), which this change does not make.

## Verification

Without a display:

```sh
python3 -B tools/test-xwayland.py
```

As root on a built system with the desktop module and the application user:

```sh
bash tools/test-xwayland.sh
```

It builds `locachy-seat` and `clipboard` from this tree, uses this tree's labwc
configuration, and runs them with a stand-in process 1, a headless labwc and
plain XCB clients, in private mount and PID namespaces, without real input
devices or the physical display. Its lines: PASS, FAIL, and GAP for the
measured limits above. A failed or unrun integration test is not a successful
deployment.
