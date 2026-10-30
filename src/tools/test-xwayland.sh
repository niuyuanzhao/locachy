#!/bin/bash
# test-xwayland.sh: the shared display's Xwayland (XWAYLAND.md), as root on a
# built system with the desktop module and the application user (uid 1000).
# locachy-seat, the clipboard and the labwc configuration are this tree's
# (built and copied into $out); labwc-control and app-init are the installed
# ones. Private mount and PID namespaces; a headless labwc started by
# locachy-seat for a stand-in process 1 (as test-labwc.sh 5), no real input,
# not the screen. Results on standard output (PASS, FAIL), the programs'
# output in $out.
#   1. display-ready comes with the display labwc's xinitrc published;
#      Xwayland runs before any X11 client, in the display's cgroup, admits
#      uid 1000 and not other users
#   2. after an idle while and after Xwayland is killed (labwc starts it
#      again) uid 1000 is still admitted
#   3. a container with x11="1": DISPLAY, that one socket, an XCB window from its own
#      network namespace (where the abstract socket is not reachable),
#      freeze, resume, stop; host-program mode; one without x11="1" has none
#   4. X11 clipboard: a container X11 copy reaches the host clipboard and
#      another X11 client; frozen and stopped, its copy stays pasteable; an
#      X11 copy between a freeze capture and its freezer write (the window
#      held open by window.py), or one the freeze cuts short, gives way to the
#      host copy of the capture
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
for tool in Xwayland xhost labwc gcc pkg-config wayland-scanner setpriv nsenter python3; do
    command -v "$tool" >/dev/null || { echo "FAIL $tool is not installed"; exit 1; }
done
for f in /usr/local/lib/locachy/labwc-control /usr/local/lib/locachy/app-init /etc/locachy/labwc; do
    [ -e "$f" ] || { echo "FAIL $f is not installed (make.sh)"; exit 1; }
done
getent passwd 1000 >/dev/null || { echo "FAIL there is no user with uid 1000"; exit 1; }
out=/var/tmp/locachy-xwayland-test
rm -rf "$out"
mkdir -p "$out/proto"
chmod 0755 "$out"
cd "$out"
gcc -O2 -Wall -Wextra -Werror "$src/tools/test-xwayland-client.c" -lxcb -o client || exit 1
base=$(ls "$src"/base/*.c | grep -v "/ime.c$")
gcc -O2 -Wall -Wextra -D_GNU_SOURCE -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -I"$src/base/include" -I"$src/base" \
    $(pkg-config --cflags freetype2 libnm) -o locachy-seat "$src/progs/seat.c" $base \
    $(pkg-config --libs freetype2 libnm) -lwayland-client -lm || exit 1
wayland-scanner client-header "$src/base/protocols/ext-data-control-v1.xml" proto/ext-data-control-v1-client-protocol.h
wayland-scanner private-code "$src/base/protocols/ext-data-control-v1.xml" proto/ext-data-control-v1-protocol.c
gcc -O2 -Wall -Wextra -Werror -I"$src/base/include" -Iproto "$src/progs/clipboard.c" "$src/base/clipboard.c" \
    proto/ext-data-control-v1-protocol.c -lwayland-client -o clipboard || exit 1
# The labwc configuration of this tree, as make.sh installs it.
cp -a /etc/locachy/labwc "$out/labwc"
install -m 644 "$src/link/labwc-rc.xml" "$out/labwc/rc.xml"
install -m 644 "$src/link/labwc-xinitrc" "$out/labwc/xinitrc"
# abstract.py N: connect to the abstract X11 socket of display N.
cat >"$out/abstract.py" <<'EOF'
import socket, sys
socket.socket(socket.AF_UNIX).connect(b"\0/tmp/.X11-unix/X" + sys.argv[1].encode())
EOF
# p1.py SEAT ARG...: process 1 for locachy-seat (test-labwc.sh).
cat >"$out/p1.py" <<'EOF'
import os, selectors, socket, sys
a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
os.environ["LOCACHY_CONTROL"] = "3"
if os.fork() == 0:
    os.dup2(b.fileno(), 3)
    os.execv(sys.argv[1], sys.argv[1:])
b.close()
null = os.open("/dev/null", os.O_RDONLY)
sel = selectors.DefaultSelector()
sel.register(a, selectors.EVENT_READ)
sel.register(sys.stdin, selectors.EVENT_READ)
while True:
    for key, _ in sel.select():
        if key.fileobj is a:
            msg = a.recv(4096)
            if not msg:
                sys.exit(0)
            print(msg.decode().replace("\0", " "), flush=True)
            if msg == b"master":
                a.send(b"mastered")
        else:
            cmd = sys.stdin.readline().strip()
            if not cmd:
                sys.exit(0)
            if cmd == "take":
                socket.send_fds(a, [b"take"], [null])
            else:
                a.send(cmd.encode())
EOF
# window.py STATE: a relay at STATE.sock for clipboard freeze
# (LOCACHY_CLIPBOARD_GUARD_SOCKET) to the compositor freeze gate: it passes
# every message on as it is, but holds "commit" (STATE.window) until STATE.go,
# so that the window between the capture and the freezer write stays open.
cat >"$out/window.py" <<'EOF'
import array, os, socket, sys, time
state = sys.argv[1]
listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
listener.bind(state + ".sock")
listener.listen(1)
open(state + ".listening", "w").close()
client, _ = listener.accept()
gate = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
gate.connect(os.environ["XDG_RUNTIME_DIR"] + "/locachy-clipboard-guard.sock")
while True:
    msg, ancillary, _, _ = client.recvmsg(128, socket.CMSG_SPACE(4))
    if not msg:
        break
    fds = array.array("i")
    for level, kind, data in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            fds.frombytes(data[:len(data) - len(data) % fds.itemsize])
    if msg == b"commit":
        open(state + ".window", "w").close()
        while not os.path.exists(state + ".go"):
            time.sleep(0.01)
    gate.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)] if fds else [])
    for fd in fds:
        os.close(fd)
    client.send(gate.recv(128))
EOF

export out src
exec timeout 300 unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mount -t tmpfs -o mode=1777 tmpfs /tmp
mount -t tmpfs tmpfs /run/udev && : >/run/udev/control
# udevadm, settings --apply, the bar and the gestures would act on the real devices.
mount --bind /usr/bin/true /usr/bin/udevadm
[ ! -e /usr/local/bin/settings ] || mount --bind /usr/bin/true /usr/local/bin/settings
[ ! -e /usr/local/bin/locachy-bar ] || mount --bind /dev/null /usr/local/bin/locachy-bar
[ ! -e /usr/local/lib/locachy/gestures ] || mount --bind /dev/null /usr/local/lib/locachy/gestures
: >$out/kmsg
[ ! -e /dev/kmsg ] || mount --bind $out/kmsg /dev/kmsg
mount --bind $out/labwc /etc/locachy/labwc
mkdir -p $out/containers $out/conf
mount -t tmpfs -o mode=0755 tmpfs $out/containers # throwaway overlays
for name in x11test x11none x11clip x11slow; do printf "x11=\"1\"\n" >$out/conf/$name.conf; done
app() { LOCACHY_CONT_BASE=$out/containers LOCACHY_CONT_CONF=$out/conf LOCACHY_APP_RUNTIME=/run/locachy/apps LOCACHY_CLIPBOARD_TOOL=$out/clipboard \
    bash $src/link/locachy-app "$@" </dev/null; }
cleanup() {
    for name in x11test x11host x11none x11off x11clip x11slow; do
        app stop $name >/dev/null 2>&1 || true
        rmdir /sys/fs/cgroup/app_$name 2>/dev/null || true
    done
}
trap cleanup EXIT
as_user() { timeout 10 setpriv --reuid=1000 --regid=1000 --clear-groups env DISPLAY=$display "$@"; }
said() { for i in $(seq 100); do grep -qx "$1" $out/p1.out && return 0; sleep 0.2; done; return 1; }
xw_pid() { pgrep -x Xwayland | head -n 1; }
wait_for() { for i in $(seq ${2:-50}); do eval "$1" && return 0; sleep 0.1; done; return 1; }

echo "== 1. the shared display starts Xwayland"
mkfifo $out/p1.in
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    python3 $out/p1.py $out/locachy-seat /usr/bin/labwc <$out/p1.in >$out/p1.out 2>$out/seat.log &
exec 8>$out/p1.in
echo take >&8
started=$(date +%s)
said display-ready
ready_after=$(( $(date +%s) - started ))
display=$(cat /run/user/0/locachy-x11-display 2>/dev/null)
check "display-ready with the X11 display xinitrc published ($display, after ${ready_after} s)" \
    "[[ \"$display\" =~ ^:[0-9]+$ ]] && [ -S /tmp/.X11-unix/X\${display#:} ] && [ $ready_after -lt 10 ]"
check "the display record is root-only" "[ \"\$(stat -c %a /run/user/0/locachy-x11-display)\" = 600 ]"
first=$(xw_pid)
check "Xwayland runs before any X11 client (xwaylandPersistence)" "[ -n \"$first\" ]"
check "Xwayland is in the cgroup of the shared display (labwc), not of an application" \
    "[ \"\$(cat /proc/$first/cgroup)\" = \"\$(cat /proc/\$(pgrep -x labwc | head -n 1)/cgroup)\" ] && ! grep -q app_ /proc/$first/cgroup"
check "uid 1000 is admitted" "as_user xhost >$out/xhost-user.log 2>&1"
check "uid 1000 connects through the filesystem socket (a network namespace of its own)" \
    "timeout 10 unshare --net setpriv --reuid=1000 --regid=1000 --clear-groups env DISPLAY=$display $out/client >$out/fs-socket.log 2>&1"
check "another user (uid 65534) is not" \
    "! timeout 10 setpriv --reuid=65534 --regid=65534 --clear-groups env DISPLAY=$display xhost >$out/xhost-other.log 2>&1"
check "the access list: root (Xwayland own) and the application user, no hosts" \
    "grep -qx \"SI:localuser:\$(getent passwd 1000 | cut -d: -f1)\" $out/xhost-user.log && ! grep -qi \"^inet\\|^local:\" $out/xhost-user.log"
WAYLAND_DISPLAY=wayland-0 $out/clipboard >$out/clipboard.log 2>&1 &
clipboard=$!
wait_for "[ -S /run/locachy/clipboard.sock ]"

echo "== 2. the user stays admitted"
sleep 12 # a lazy Xwayland ends 10 s after its last client
check "idle without clients: the same Xwayland ($first), the user admitted" \
    "[ \"\$(xw_pid)\" = \"$first\" ] && as_user xhost >/dev/null 2>&1"
kill -KILL "$first"
wait_for "[ -n \"\$(xw_pid)\" ] && [ \"\$(xw_pid)\" != $first ]" 50
second=$(xw_pid)
wait_for "as_user xhost >/dev/null 2>&1" 50
check "killed: labwc started Xwayland again ($second), on $display, the user admitted again" \
    "[ -n \"$second\" ] && [ \"$second\" != \"$first\" ] && [ \"\$(cat /run/user/0/locachy-x11-display)\" = \"$display\" ] && as_user xhost >/dev/null 2>&1"

echo "== 3. containers"
app start x11test $out/client --hold >$out/app.log 2>&1 &
launcher=$!
wait_for "grep -q \"connected $display uid=1000\" $out/app.log" 120
check "a container XCB window as uid 1000 on $display" "grep -q \"connected $display uid=1000\" $out/app.log"
pid=$(cut -d" " -f1 /run/locachy/apps/x11test/reaper.pid 2>/dev/null)
tr "\0" "\n" </proc/$(pgrep -f "^$out/client --hold" | head -n 1)/environ >$out/environ 2>/dev/null
check "its DISPLAY is the shared one, no XAUTHORITY" "grep -qx DISPLAY=$display $out/environ && ! grep -q ^XAUTHORITY= $out/environ"
check "its /tmp/.X11-unix has only that socket" \
    "[ \"\$(timeout 5 nsenter --target $pid --mount --root -- find /tmp/.X11-unix -mindepth 1 -maxdepth 1 -printf \"%f\\n\")\" = X\${display#:} ]"
check "the abstract socket: reachable in the host network namespace, not in the container one" \
    "[ -n \"$pid\" ] && python3 $out/abstract.py \${display#:} && ! timeout 5 nsenter --target $pid --net -- python3 $out/abstract.py \${display#:} 2>/dev/null"
check "a second client in its own network namespace connects (the filesystem socket)" \
    "[ \"\$(readlink /proc/$pid/ns/net)\" != \"\$(readlink /proc/1/ns/net)\" ] && timeout 5 nsenter --target $pid --mount --net --root -- env DISPLAY=$display setpriv --reuid=1000 --regid=1000 --clear-groups $out/client >$out/second.log 2>&1 && grep -q \"connected $display uid=1000\" $out/second.log"
app freeze x11test >>$out/app.log 2>&1
check "freeze" "grep -q \"frozen 1\" /sys/fs/cgroup/app_x11test/cgroup.events"
app resume x11test >>$out/app.log 2>&1
check "resume" "grep -q \"frozen 0\" /sys/fs/cgroup/app_x11test/cgroup.events"
app stop x11test >>$out/app.log 2>&1
wait $launcher 2>/dev/null
check "stop leaves its cgroup empty" "! grep -q \"populated 1\" /sys/fs/cgroup/app_x11test/cgroup.events"
printf "host=\"1\"\nx11=\"1\"\n" >$out/conf/x11host.conf
app start x11host $out/client >$out/host.log 2>&1
check "a host program gets the same display" "grep -q \"connected $display uid=1000\" $out/host.log"
printf ":99\n" >/run/user/0/locachy-x11-display # its socket is not there
app start x11none $out/client >$out/none.log 2>&1
printf "%s\n" "$display" >/run/user/0/locachy-x11-display
check "a record without its socket: the container still starts, without DISPLAY" \
    "grep -q \"starts without X11\" $out/none.log && grep -q \"DISPLAY=unset\" $out/none.log"
app start x11off $out/client >$out/off.log 2>&1 # its configuration does not ask for X11
check "a container without x11=\"1\": no DISPLAY, Wayland only" \
    "grep -q \"DISPLAY=unset\" $out/off.log && ! grep -q \"without X11\" $out/off.log"

echo "== 4. the X11 clipboard"
get() { timeout 6 $out/clipboard get 2>&1; }
paste() { as_user $out/client paste 2>&1 | sed -n "s/^pasted //p"; }
app start x11clip $out/client copy x11 >$out/clip.log 2>&1 &
clip_launcher=$!
wait_for "[ \"\$(get)\" = x11-1 ]" 100
check "an X11 copy in a container reaches the host clipboard (x11-1)" "[ \"\$(get)\" = x11-1 ]"
check "and another X11 client pastes it" "[ \"\$(paste)\" = x11-1 ]"
check "the host clipboard took it over: the container no longer owns it" "grep -q \"lost the selection\" $out/clip.log"
app freeze x11clip >>$out/clip.log 2>&1
check "frozen (clipboard freeze): the copy is still the clipboard, pasteable in X11" \
    "grep -q \"frozen 1\" /sys/fs/cgroup/app_x11clip/cgroup.events && [ \"\$(get)\" = x11-1 ] && [ \"\$(paste)\" = x11-1 ]"
app resume x11clip >>$out/clip.log 2>&1
pkill -USR1 -f "^$out/client copy x11"
wait_for "[ \"\$(get)\" = x11-2 ]" 50
check "resumed, it copies again (x11-2)" "[ \"\$(get)\" = x11-2 ]"
# freeze_window NAME STATE: locachy-app freeze NAME, its window between the
# capture and the freezer write held open by window.py until STATE.go.
freeze_window() {
    python3 $out/window.py $out/$2 >$out/$2.log 2>&1 &
    wait_for "[ -e $out/$2.listening ]"
    ( export LOCACHY_CLIPBOARD_GUARD_SOCKET=$out/$2.sock; app freeze $1 ) >>$out/$2.log 2>&1 &
    freezer=$!
    wait_for "[ -e $out/$2.window ]" 60
}
printf retained-by-host | $out/clipboard set
freeze_window x11clip w1
pkill -USR1 -f "^$out/client copy x11"
wait_for "grep -q \"^copied x11-3\" $out/clip.log" 20
sleep 1
inside=$(get)
touch $out/w1.go
wait $freezer; frozen=$?
got=$(get); x11=$(paste)
check "an X11 copy between the freeze capture and its freezer write (clipboard then \"$inside\") gives way to the host copy: \"$got\", X11 paste \"$x11\"" \
    "grep -q \"^copied x11-3\" $out/clip.log && [ $frozen = 0 ] && grep -q \"frozen 1\" /sys/fs/cgroup/app_x11clip/cgroup.events && [ \"$got\" = retained-by-host ] && [ \"$x11\" = retained-by-host ]"
app resume x11clip >>$out/clip.log 2>&1
app stop x11clip >>$out/clip.log 2>&1
wait $clip_launcher 2>/dev/null
last=$(get)
check "stopped: its last copy stays the clipboard, pasteable in X11 ($last)" \
    "grep -q \"^copied x11-1\" $out/clip.log && [ \"$last\" != \"\" ] && [ \"\$(paste)\" = \"$last\" ]"
# A source slow to answer (300 ms a conversion), frozen (locachy-app freeze)
# just after its copy, while the host clipboard is still reading it.
app start x11slow $out/client copy slow 300 >$out/slow.log 2>&1 &
slow_launcher=$!
wait_for "[ \"\$(get)\" = slow-1 ]" 100
pkill -USR1 -f "^$out/client copy slow"
sleep 0.3
if app freeze x11slow >>$out/slow.log 2>&1; then
    got=$(get); x11=$(paste)
    check "frozen during a slow X11 transfer, the freeze waited for it (clipboard $got, X11 paste $x11)" \
        "grep -q \"frozen 1\" /sys/fs/cgroup/app_x11slow/cgroup.events && [ \"$got\" = slow-2 ] && [ \"$x11\" = slow-2 ]"
else
    got=$(get)
    check "freeze refused during a slow X11 transfer: not frozen, the clipboard intact ($got)" \
        "grep -q \"frozen 0\" /sys/fs/cgroup/app_x11slow/cgroup.events && [ \"${got%-*}\" = slow ]"
fi
# The same window: the slow source copies inside it, and the freezer write
# comes while the host clipboard is still reading that copy.
app resume x11slow >>$out/slow.log 2>&1
before=$(get)
freeze_window x11slow w2
pkill -USR1 -f "^$out/client copy slow"
wait_for "grep -q \"^copied slow-3\" $out/slow.log" 20
sleep 0.2
touch $out/w2.go
wait $freezer; frozen=$?
got=$(get); x11=$(paste)
check "the freeze cut the transfer of an X11 copy short: the host copy from the capture stays (\"$before\": \"$got\", X11 paste \"$x11\")" \
    "[ $frozen = 0 ] && grep -q \"frozen 1\" /sys/fs/cgroup/app_x11slow/cgroup.events && [ -n \"$before\" ] && [ \"$got\" = \"$before\" ] && [ \"$x11\" = \"$before\" ]"
app resume x11slow >>$out/slow.log 2>&1
pkill -USR1 -f "^$out/client copy slow"
wait_for "[ \"\$(get)\" = slow-4 ]" 50
check "resumed, the host clipboard takes its next X11 copy (slow-4)" "[ \"\$(get)\" = slow-4 ]"
app stop x11slow >>$out/slow.log 2>&1
wait $slow_launcher 2>/dev/null

kill $clipboard
pkill -x labwc
exec 8>&-
# The seat is a child of the stand-in process 1, not of this shell.
for i in $(seq 100); do [ ! -e /run/user/0/locachy-x11-display ] && break; sleep 0.1; done
check "the seat removed the display record at its end" "[ ! -e /run/user/0/locachy-x11-display ]"
wait 2>/dev/null
[ $fail = 0 ] && echo "ALL PASSED (logs: $out)" || echo "SOME FAILED (logs: $out)"
exit $fail
'
