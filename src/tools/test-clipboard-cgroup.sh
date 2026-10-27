#!/bin/bash
# Real cgroup freezer + clipboard barrier on a private headless display.
# The group is nested below a non-app test group so process 1 never treats
# it as an application or opens the real display. Run test-clipboard.sh
# first to build the two candidate programs used here.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
labwc=${1:-/usr/bin/labwc}
out=${LOCACHY_CLIPBOARD_TEST_OUT:-$(mktemp -d /var/tmp/locachy-clipboard-cgroup-test.XXXXXX)}
[ -x "$out/clipboard" ] && [ -x "$out/client" ] || \
    LOCACHY_CLIPBOARD_TEST_OUT="$out" LOCACHY_TEST_LABWC="$labwc" bash "$src/tools/test-clipboard.sh"
group=$(mktemp -d /sys/fs/cgroup/locachy-clipboard-test.XXXXXX)
cleanup() {
    if [ -d "$group/app_source" ]; then
        printf '0\n' >"$group/app_source/cgroup.freeze"
        printf '1\n' >"$group/app_source/cgroup.kill"
        rmdir "$group/app_source" 2>/dev/null || true
    fi
    rmdir "$group" 2>/dev/null || true
}
trap cleanup EXIT
mkdir "$group/app_source"
# Definitions only: the real freeze function with the test cgroup root.
sed '/^operation=start/,$d' "$src/link/locachy-app" >"$out/freeze-functions.sh"
export out src group labwc
unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs tmpfs /run/user
    mount -t tmpfs tmpfs /run/locachy
    install -d -m 0711 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    export LOCACHY_CLIPBOARD_SOCKET=/run/locachy/clipboard.sock
    export LOCACHY_CLIPBOARD_TOOL=$out/clipboard LOCACHY_APP_RUNTIME=/run/locachy/apps
    mkdir -p /run/locachy/apps/source
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
        "$labwc" -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >"$out/cgroup-labwc.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
    "$out/clipboard" >"$out/cgroup-service.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S "$LOCACHY_CLIPBOARD_SOCKET" ] && break; sleep 0.1; done
    (echo 0 >"$group/app_source/cgroup.procs"; exec "$out/client" source --slow cgroup) \
        >"$out/cgroup-source.out" 2>"$out/cgroup-source.log" &
    source=$!
    for _ in $(seq 1 50); do grep -q ready "$out/cgroup-source.log" && break; sleep 0.1; done
    . "$out/freeze-functions.sh"
    CGROUP_BASE=$group
    # Reclaim is not relevant to the clipboard contract; exercise the actual
    # freezer and its events without starting an unrelated reclaim job.
    bytes_of() { echo 0; }
    do_freeze source
    grep -qx "frozen 1" "$group/app_source/cgroup.events"
    timeout 4 "$out/clipboard" get | grep -q "cgroup 中文 🌍"
    timeout 4 "$out/client" read image/png >"$out/cgroup-image"
    test "$(wc -c <"$out/cgroup-image")" = 12
    echo "PASS actual cgroup freeze waits for complete clipboard takeover; frozen source is not contacted"
    do_resume source
    grep -qx "frozen 0" "$group/app_source/cgroup.events"
    kill -TERM "$source"
    wait "$source" 2>/dev/null || true
    timeout 4 "$out/clipboard" get | grep -q "cgroup 中文 🌍"
    echo "PASS resume and source exit preserve the saved clipboard"
    echo "ALL PASSED"
'
