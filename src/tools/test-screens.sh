#!/bin/bash
# Isolated screen arrangement integration: no uinput, physical input, DRM
# device, host configuration or running compositor is used. Read back actual
# headless output state rather than treating a sent request as success.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${LOCACHY_SCREENS_TEST_OUT:-$(mktemp -d /var/tmp/locachy-screens-test.XXXXXX)}
if [ -e "$out" ] && [ -n "$(find "$out" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    printf 'Output directory must be empty: %s\n' "$out" >&2
    exit 2
fi
mkdir -p "$out/proto"
out=$(realpath "$out")
for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 \
         ext-idle-notify-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/proto/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/proto/$p-protocol.c"
done
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections
    -I"$src/base/include" -I"$src/base" -I"$out/proto")
protocols=("$out"/proto/*-protocol.c)
"${cc[@]}" $(pkg-config --cflags gio-2.0) "$src/progs/labwc-control.c" "$src/base/say.c" "$src/base/open.c" \
    "$src/base/rc.c" "$src/base/monitors.c" "${protocols[@]}" -Wl,--gc-sections -lwayland-client \
    $(pkg-config --libs gio-2.0) -lm -o "$out/control"
"${cc[@]}" "$src/tools/test-output-state.c" "${protocols[@]}" \
    -Wl,--gc-sections -lwayland-client -lm -o "$out/state"
"${cc[@]}" "$src/tools/test-monitor-layout.c" -o "$out/layout-test"
"$out/layout-test"
"${cc[@]}" "$src/tools/test-settings-monitors.c" -Wl,--gc-sections -o "$out/settings-test"
"$out/settings-test" "$out/ui-fixtures"
export src out
exec timeout 120 unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs tmpfs /run/user
    mount -t tmpfs tmpfs /run/locachy
    install -d -m 0700 /run/user/0
    mkdir -p "$out/etc/labwc"
    printf "<labwc_config/>\n" >"$out/etc/labwc/rc.xml"
    mount --bind "$out/etc" /etc/locachy
    # Hide physical input and DRM even if an accidental backend is selected.
    mount -t tmpfs -o mode=0755 tmpfs /dev/input
    mount -t tmpfs -o mode=0755 tmpfs /dev/dri
    mount --bind /dev/full /dev/kmsg
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    export XDG_CONFIG_HOME="$out/etc" HOME="$out" XDG_CONFIG_DIRS="$out/etc"
    comp= control=
    trap '\''kill ${control:-} ${comp:-} 2>/dev/null || true'\'' EXIT
    alive() {
        kill -0 "$comp" || { printf "FAIL compositor exited\n"; return 1; }
        if [ -n "$control" ]; then kill -0 "$control" || { printf "FAIL controller exited\n"; return 1; }; fi
    }
    request() {
        timeout 4 python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET); s.settimeout(2); s.connect(sys.argv[1]); s.send(b\"reload-monitors\"); print(s.recv(256).decode())" "$XDG_RUNTIME_DIR/locachy-control.sock"
    }
    config() {
        "$out/settings-test" --fixture-dir "$out/fixture-$1" --place "$1"
        cp "$out/fixture-$1/monitors.conf" /etc/locachy/monitors.conf.tmp
        mv /etc/locachy/monitors.conf.tmp /etc/locachy/monitors.conf
    }
    state_matches() {
        timeout 3 "$out/state" >"$out/state-$1.txt" || return 1
        python3 -c "import sys
rows={r[0]:r[1:] for r in map(str.split,open(sys.argv[1]))}
a,b=rows.get(\"HEADLESS-1\"),rows.get(\"HEADLESS-2\")
expected={\"right\":((0,0),(1024,0)),\"left\":((853,0),(0,0)),\"above\":((0,480),(0,0)),\"below\":((0,0),(0,576))}
if not a or not b or len(rows)!=2: sys.exit(1)
for row,xy,scale in ((a,expected[sys.argv[2]][0],1.25),(b,expected[sys.argv[2]][1],1.5)):
 if int(row[0])!=1 or tuple(map(int,row[1:3]))!=xy or tuple(map(int,row[3:5]))!=(1280,720) or abs(float(row[5])-scale)>0.001: sys.exit(1)
" "$out/state-$1.txt" "$1"
    }
    wait_state() {
        for _ in $(seq 1 50); do
            alive
            if state_matches "$1"; then
                printf "PASS UI -> saved config -> compositor: %s\n" "$1"
                cat "$out/state-$1.txt"
                return 0
            fi
            sleep 0.1
        done
        cat "$out/state-$1.txt" "$out/control.log" "$out/labwc.log"
        printf "FAIL actual output geometry: %s\n" "$1"
        return 1
    }
    start_control() {
        "$out/control" >>"$out/control.log" 2>&1 &
        control=$!
        for _ in $(seq 1 50); do
            alive
            [ -S "$XDG_RUNTIME_DIR/locachy-control.sock" ] && return 0
            sleep 0.1
        done
        return 1
    }
    config right
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 \
        labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >"$out/labwc.log" 2>&1 &
    comp=$!
    for _ in $(seq 1 50); do
        alive
        [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break
        sleep 0.1
    done
    [ -S "$XDG_RUNTIME_DIR/wayland-0" ]
    start_control
    wait_state right
    config left
    [ "$(request)" = "ok monitors" ]
    wait_state left
    config above
    wait_state above
    config below
    wait_state below
    # Reload from the saved file after both display and controller restart.
    kill -TERM "$control"; wait "$control"; control=
    kill -TERM "$comp"; wait "$comp"; comp=
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 \
        labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >>"$out/labwc.log" 2>&1 &
    comp=$!
    for _ in $(seq 1 50); do alive; [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break; sleep 0.1; done
    start_control
    wait_state below
    printf "PASS saved arrangement survives controller and compositor restart\n"
    printf "Screen test results: %s\n" "$out"
'
