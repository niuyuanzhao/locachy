#!/bin/bash
# The host manager sees data-control; a security-context client does not.
# A private headless display and socket mount, never the real session.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d /var/tmp/locachy-clipboard-restricted-test.XXXXXX)
labwc=${LOCACHY_TEST_LABWC:-/usr/bin/labwc}
clip=${LOCACHY_TEST_CLIPBOARD:-/usr/local/lib/locachy/clipboard}
mkdir -p "$out"
gcc -O2 -Wall -Wextra -Werror "$src/tools/wayland-check.c" -lwayland-client -o "$out/check"
for p in xdg-shell virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1 primary-selection-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/$p-protocol.c"
done
gcc -O2 -Wall -Wextra -Werror -I"$out" "$src/tools/test-clipboard-ordinary.c" \
    "$out/xdg-shell-protocol.c" "$out/primary-selection-unstable-v1-protocol.c" -lwayland-client -o "$out/ordinary"
gcc -O2 -Wall -Wextra -Werror -I"$out" "$src/tools/test-seat.c" \
    "$out/virtual-keyboard-unstable-v1-protocol.c" "$out/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -lwayland-client -lxkbcommon -o "$out/seat"
export out labwc clip
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs tmpfs /run/user
    mount -t tmpfs tmpfs /run/locachy
    mkdir -m 0711 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
        "$labwc" -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >"$out/labwc.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
    "$out/seat" >"$out/seat.log" 2>&1 &
    input=$!
    /usr/local/lib/locachy/labwc-control >"$out/control.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
    timeout 5 "$out/check" >"$out/host"
    WAYLAND_DISPLAY=locachy-0 timeout 5 "$out/check" >"$out/restricted"
    grep -q ext_data_control_manager_v1 "$out/host"
    grep -q wl_data_device_manager "$out/restricted"
    ! grep -q data_control_manager "$out/restricted"
    echo "PASS host has data-control; restricted clients have ordinary copy/paste but no data-control"
    export LOCACHY_CLIPBOARD_SOCKET=/run/locachy/clipboard.sock
    "$clip" >"$out/clipboard.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S "$LOCACHY_CLIPBOARD_SOCKET" ] && break; sleep 0.1; done
    WAYLAND_DISPLAY=locachy-0 "$out/ordinary" source >"$out/source.log" 2>"$out/source.err" &
    source=$!
    for _ in $(seq 1 50); do grep -q activated "$out/source.log" && break; sleep 0.1; done
    kill -USR1 "$input"
    for _ in $(seq 1 50); do
        if "$clip" get 2>/dev/null | grep -q "restricted ordinary"; then break; fi
        sleep 0.1
    done
    "$clip" sync >/dev/null
    kill -TERM "$source"
    wait "$source" 2>/dev/null || true
    WAYLAND_DISPLAY=locachy-0 timeout 8 "$out/ordinary" read >"$out/paste.log" 2>"$out/paste.err"
    grep -q "ordinary paste received" "$out/paste.log"
    echo "PASS ordinary restricted source copied on key serial; restricted target pasted after source exit"
    echo "ALL PASSED"
'
