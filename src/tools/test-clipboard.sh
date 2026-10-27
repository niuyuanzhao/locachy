#!/bin/bash
# Clipboard tests in a private mount/PID namespace, with a headless labwc:
# no host clipboard, input device, or display is touched. Generated fixtures
# only, no history files. The test's result files are synthetic payloads.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${LOCACHY_CLIPBOARD_TEST_OUT:-$(mktemp -d /var/tmp/locachy-clipboard-test.XXXXXX)}
labwc=${LOCACHY_TEST_LABWC:-/usr/bin/labwc}
mkdir -p "$out/proto"
wayland-scanner client-header "$src/base/protocols/ext-data-control-v1.xml" "$out/proto/ext-data-control-v1-client-protocol.h"
wayland-scanner private-code "$src/base/protocols/ext-data-control-v1.xml" "$out/proto/ext-data-control-v1-protocol.c"
cc=(gcc -O2 -g -Wall -Wextra -Werror -I"$src/base/include" -I"$out/proto")
"${cc[@]}" "$src/progs/clipboard.c" "$src/base/clipboard.c" "$out/proto/ext-data-control-v1-protocol.c" -lwayland-client -o "$out/clipboard"
"${cc[@]}" "$src/tools/test-clipboard-client.c" "$out/proto/ext-data-control-v1-protocol.c" -lwayland-client -o "$out/client"
export src out labwc
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs tmpfs /run/user
    mount -t tmpfs tmpfs /run/locachy
    install -d -m 0711 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    export LOCACHY_CLIPBOARD_SOCKET=/run/locachy/clipboard.sock
    start_display() {
        WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
            "$labwc" -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >"$out/labwc.log" 2>&1 &
        comp=$!
        for _ in $(seq 1 60); do [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break; sleep 0.1; done
    }
    clip() { timeout 4 "$out/clipboard" "$@"; }
    source() {
        "$out/client" source "$@" >"$out/source.out" 2>"$out/source.log" &
        source=$!
        for _ in $(seq 1 50); do grep -q ready "$out/source.log" && break; sleep 0.05; done
    }
    sync_clip() {
        for _ in $(seq 1 60); do
            if clip sync >"$out/token" 2>"$out/sync.log"; then return 0; fi
            sleep 0.05
        done
        cat "$out/sync.log"
        return 1
    }
    start_display
    "$out/clipboard" >"$out/service.log" 2>&1 &
    daemon=$!
    for _ in $(seq 1 50); do [ -S "$LOCACHY_CLIPBOARD_SOCKET" ] && break; sleep 0.1; done
    for _ in $(seq 1 50); do clip status | grep -q wayland=connected && break; sleep 0.1; done

    source first
    sync_clip
    wait "$source"
    clip get >"$out/text"
    grep -q "first 中文 🌍" "$out/text"
    timeout 4 "$out/client" read text/html >"$out/html"
    grep -q "<p>first 中文 🌍</p>" "$out/html"
    timeout 4 "$out/client" read image/png >"$out/png"
    test "$(wc -c <"$out/png")" = 12
    echo "PASS text, HTML and binary survive source exit"

    source --primary primary
    sync_clip
    kill -TERM "$source"
    wait "$source" 2>/dev/null || true
    clip get --primary >"$out/primary"
    grep -q "primary 中文 🌍" "$out/primary"
    clip get | grep -q "first 中文 🌍"
    echo "PASS primary and regular selections remain independent"

    kill -STOP "$comp"
    for i in $(seq 1 40); do printf "intermediate %s\n" "$i" | clip set; done
    printf "from term 中文 🌍\n" | clip set
    clip get | grep -q "from term 中文 🌍"
    test "$(find /proc/$daemon/fd -maxdepth 1 -type l | wc -l)" -lt 40
    kill -CONT "$comp"
    sync_clip
    timeout 4 "$out/client" read text/plain >"$out/from-term"
    grep -q "from term 中文 🌍" "$out/from-term"
    echo "PASS local text read/write works while compositor is frozen"

    kill -TERM "$comp"
    wait "$comp"
    clip get | grep -q "from term 中文 🌍"
    start_display
    for _ in $(seq 1 60); do
        clip status | grep -q wayland=connected && break
        sleep 0.1
    done
    sync_clip
    timeout 4 "$out/client" read text/plain >"$out/restored"
    grep -q "from term 中文 🌍" "$out/restored"
    echo "PASS content survives compositor exit and reconnect"

    timeout 4 "$out/client" clear
    sync_clip
    if clip get >"$out/empty" 2>/dev/null; then exit 1; else test "$?" = 1; fi
    printf "" | clip set
    clip get >"$out/zero"
    test ! -s "$out/zero"
    echo "PASS clear and successful zero-length text are distinct"

    source --slow frozen
    sync_clip
    kill -STOP "$source"
    clip get | grep -q "frozen 中文 🌍"
    timeout 4 "$out/client" read text/html | grep -q "<p>frozen 中文 🌍</p>"
    kill -CONT "$source"
    kill -TERM "$source"
    wait "$source" 2>/dev/null || true
    echo "PASS a stopped source is not needed to serve any saved MIME"

    source --slow older
    source1=$source
    "$out/client" source newer >"$out/newer.out" 2>"$out/newer.log" &
    source2=$!
    sync_clip
    clip get | grep -q "newer 中文 🌍"
    kill -TERM "$source1" "$source2" 2>/dev/null || true
    wait "$source1" "$source2" 2>/dev/null || true
    echo "PASS a superseded slow source cannot overwrite new content"

    source --hang broken
    if clip sync >"$out/hung" 2>/dev/null; then exit 1; fi
    if clip get >"$out/broken" 2>/dev/null; then exit 1; fi
    printf recovered | clip set
    clip get | grep -qx recovered
    kill -TERM "$source" 2>/dev/null || true
    wait "$source" 2>/dev/null || true
    echo "PASS a non-responsive source times out; local interface remains usable"

    kill -TERM "$daemon"
    wait "$daemon"
    kill -TERM "$comp"
    wait "$comp"
    echo "ALL PASSED"
'
