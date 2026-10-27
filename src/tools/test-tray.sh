#!/bin/bash
# test-tray.sh: programs in a tray, as root on a built system with the user
# (uid 1000) made. On a headless labwc in private mount and PID namespaces,
# with throwaway containers (the real ones and /run/locachy are hidden behind
# copies), typing from a uinput keyboard and clicking with a virtual pointer.
# It writes files named .locachy-tray-test* in the shared Documents and
# removes them.
#   1. a container has a bus of its own; the bar is the tray watcher on it
#      before the program starts
#   2. a program with a tray item and no window is in the window list, marked
#      when it asks for attention
#   3. opening it (show, as from home) asks its item for the window; so does
#      a click on its button; when no window comes, whether to start it again
#      is asked
#   4. a program with no window and no tray item: opening it asks first; Esc
#      leaves it, Enter starts its program again
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-tray-test # not /tmp: a container has a /tmp of its own
rm -rf "$out"
mkdir -p "$out"
IFS=: read -r user _ uid gid _ uhome _ <<<"$(getent passwd 1000)"
[ -n "${uid:-}" ] || { echo "no user with uid 1000"; exit 1; }
docs=$uhome/Documents
cd "$out"

for p in wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
         wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter \
         wlr-screencopy-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
cc="gcc -O2 -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base -I$out $(pkg-config --cflags freetype2 libnm)"
base=$(ls "$src"/base/*.c | grep -v '/ime.c$')
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_IME -o locachy-bar "$src/progs/bar.c" $base \
    "$src/base/ime.c" wlr-layer-shell-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
    xdg-shell-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c $(pkg-config --libs freetype2 libnm) \
    -lwayland-client -lwayland-cursor -lrime -lxkbcommon || exit 1
$cc $(pkg-config --cflags gio-2.0) -o test-tray-item "$src/tools/test-tray-item.c" $(pkg-config --libs gio-2.0) || exit 1
$cc -o test-uinput "$src/tools/test-uinput.c" || exit 1
$cc -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-click "$src/tools/test-click.c" wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1
modprobe uinput 2>/dev/null || true

# A program with no window and no tray item: a line in its log for each start.
cat >"$out/plain" <<'EOF'
#!/bin/sh
echo started >>"$1"
sleep 120
EOF
chmod 755 "$out/plain"
# colour.py FILE RRGGBB [ROW]: how many pixels have the colour (in ROW, or
# everywhere), and the middle of those in ROW: "COUNT X".
cat >colour.py <<'EOF'
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w, h = map(int, d[1].split())
px, c = d[3], bytes.fromhex(sys.argv[2])
rows = [int(sys.argv[3])] if len(sys.argv) > 3 else range(h)
xs = [x for y in rows for x in range(w) if px[(y * w + x) * 3:(y * w + x) * 3 + 3] == c]
print(len(xs), (xs[0] + xs[-1]) // 2 if xs else -1)
EOF

export out src uid docs
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mount -t tmpfs tmpfs /var/lib/locachy/containers
C=/var/lib/locachy/containers
mkdir -p /etc/locachy/containers
mount -t tmpfs tmpfs /etc/locachy/containers # no real container is among them
mk() { # NAME COMMAND...
    local name=$1; shift
    mkdir -p $C/$name/upper/usr/share/applications
    printf "command=\"%s\"\n" "$*" >/etc/locachy/containers/$name.conf
    printf "[Desktop Entry]\nType=Application\nName=Test %s\nExec=%s\n" "$name" "$*" \
        >$C/$name/upper/usr/share/applications/$name.desktop
}
f=$docs/.locachy-tray-test
mk trayapp $out/test-tray-item $f.tray attention
mk plain $out/plain $f.plain

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
/usr/local/lib/locachy/labwc-control >$out/control.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
coproc KEYS { $out/test-uinput 2>$out/uinput.log; }
read -r dev <&${KEYS[0]}
mount -t tmpfs -o mode=0755 tmpfs /dev/input
mknod /dev/input/event0 c ${dev%%:*} ${dev##*:}
key() { for k in "$@"; do echo "$k" >&${KEYS[1]}; done; sleep 0.4; }
$out/locachy-bar >$out/bar.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-bar.sock ] && break; sleep 0.1; done
request() { python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(\"/run/user/0/locachy-control.sock\")
s.send(sys.argv[1].encode())
print(s.recv(256).decode())" "$1"; }
shot() { $out/test-screenshot $out/shot 2>>$out/client.log; }
count() { python3 $out/colour.py $out/shot-HEADLESS-1.ppm "$@"; }
activated() { grep -c "^activated$" $f.tray 2>/dev/null; }
started() { grep -c "^started$" $f.plain 2>/dev/null; }

echo "== 1. a bus of its own, the bar its tray watcher"
LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app start trayapp >$out/trayapp.log 2>&1 &
LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app start plain >$out/plain.log 2>&1 &
for i in $(seq 40); do grep -q registered $f.tray 2>/dev/null && break; sleep 0.25; done
check "the container has a bus of its own" "[ -S /run/locachy/apps/trayapp/bus/bus ]"
check "the watcher was there when the program started" "grep -qx \"watcher yes\" $f.tray"
check "its item registered with it" "grep -qx registered $f.tray"

echo "== 2. in the window list, marked"
sleep 1
shot
b=$(count b0702a 18)
check "the window list has it, marked for attention ($b)" "[ ${b%% *} -gt 100 ]"
x=${b##* }

echo "== 3. its window, from its tray item"
reply=$(request "show trayapp")
sleep 0.5
check "show trayapp: from its tray ($reply)" "echo \"$reply\" | grep -q \"from the tray\""
check "the item was asked for the window (Activate)" "[ \"$(activated)\" = 1 ]"
$out/test-click $x 18 1280 720 2>>$out/client.log
sleep 0.5
check "a click on its button asks again" "[ \"$(activated)\" = 2 ]"
sleep 3.2
shot
o=$(count 182848)
check "no window came from its item: asked whether to start it again" "[ ${o%% *} -gt 50000 ]"
key 1
sleep 1
shot
o=$(count 182848)
check "Esc leaves it" "[ ${o%% *} -lt 1000 ]"

echo "== 4. no window, no tray item: asked first"
for i in $(seq 20); do [ "$(started)" = 1 ] && break; sleep 0.25; done
reply=$(request "show plain")
sleep 0.7
shot
o=$(count 182848)
check "show plain asks ($reply)" "echo \"$reply\" | grep -q asking && [ ${o%% *} -gt 50000 ]"
key 1
sleep 1
shot
o=$(count 182848)
check "Esc leaves it: no second start, the question gone" "[ \"$(started)\" = 1 ] && [ ${o%% *} -lt 1000 ]"
request "show plain" >/dev/null
sleep 0.7
key 28
for i in $(seq 20); do [ "$(started)" = 2 ] && break; sleep 0.25; done
check "Enter starts its program again" "[ \"$(started)\" = 2 ]"

grep -h "locachy-bar:" <(dmesg) | grep -E "tray|again|without" | tail -8
rm -f $f.*
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -20 $out/bar.log; tail -5 $out/trayapp.log $out/plain.log; }
eval "exec ${KEYS[1]}>&-"
for c in trayapp plain; do LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app stop $c >/dev/null 2>&1; rmdir /sys/fs/cgroup/app_$c 2>/dev/null; done
exit $fail
'
