#!/bin/bash
# test-container-window.sh NAME [ARGUMENT...]: does container NAME's command
# (with ARGUMENT... as its arguments, if given) open a window on a headless
# labwc? As root on a built system, NAME built and closed. It runs as a
# throwaway container, tNAME, over NAME's build (its upper) with a home of
# its own (in $out): process 1 does not know it, so it neither shows the
# shared display for it nor touches NAME; the screen and /run/user/0 are not
# used (private mount and PID namespaces, as test-isolation.sh). Its seat has
# a keyboard and a pointer (test-seat), as the real one. On the network if
# NAME is. Prints the app_id of each window it opens within 60 s
# and keeps a screenshot of the screen then (shot-*.ppm in $out).
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
name=${1:?usage: test-container-window.sh NAME [ARGUMENT...]}
shift
out=/var/tmp/locachy-window-test
real=/var/lib/locachy/containers/$name
conf=/etc/locachy/containers/$name.conf
[ -f "$conf" ] && [ -f "$real/built" ] || { echo "$name is not built"; exit 1; }
if grep -q "populated 1" /sys/fs/cgroup/app_$name/cgroup.events 2>/dev/null; then
    echo "$name is running: close it first"
    exit 1
fi
rm -rf "$out"
mkdir -p "$out/conf" "$out/containers/t$name/merged" "$out/containers/t$name/data"
ln -s "$real/upper" "$out/containers/t$name/upper"
ln -s "$real/work" "$out/containers/t$name/work"
cp "$conf" "$out/conf/t$name.conf"
if [ $# -gt 0 ]; then
    sed -i '/^arguments=/d' "$out/conf/t$name.conf"
    printf 'arguments="%s"\n' "$*" >>"$out/conf/t$name.conf"
fi
cd "$out"
for p in wlr-screencopy-unstable-v1 virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
gcc -O2 -I"$out" -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
gcc -O2 -I"$out" -o test-seat "$src/tools/test-seat.c" virtual-keyboard-unstable-v1-protocol.c \
    wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client -lxkbcommon || exit 1
touch kmsg

export out src name
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mount --bind $out/kmsg /dev/kmsg # what labwc-control says (say) goes here, not to the log
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
WAYLAND_DISPLAY=wayland-0 $out/test-seat 2>>$out/control.log & # a keyboard and a pointer, as the real seat
WAYLAND_DISPLAY=wayland-0 /usr/local/lib/locachy/labwc-control >$out/control.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
app() { LOCACHY_CONT_BASE=$out/containers LOCACHY_CONT_CONF=$out/conf LOCACHY_APP_RUNTIME=/run/locachy/apps \
    bash $src/link/locachy-app "$@" </dev/null; }
app start t$name >$out/app.log 2>&1 &
for i in $(seq 120); do grep -q "window opened" $out/kmsg && break; sleep 0.5; done
sleep 8 # its window drawn
WAYLAND_DISPLAY=wayland-0 $out/test-screenshot $out/shot 2>>$out/control.log
if grep -q "window opened" $out/kmsg; then
    sed -n "s/.*window opened, app_id /window: /p" $out/kmsg | sort -u
else
    echo "no window in 60 s; its output:"
    tail -15 $out/app.log
fi
app stop t$name >/dev/null 2>&1
rmdir /sys/fs/cgroup/app_t$name 2>/dev/null || true
'
