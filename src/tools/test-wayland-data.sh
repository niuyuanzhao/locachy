#!/bin/bash
# test-wayland-data.sh NEW_OUTPUT: base/wayland-data.c in a real window of a
# headless labwc. Its clipboard and primary selection meet a data-control
# client; its text/uri-list drag is dropped on foot, a separate program. Run
# as root in the development root: private mount/PID namespace, no host
# display, clipboard or input device. Also the write queue's unit checks.
# With LOCachy's labwc, the development root must expose /sys/fs/cgroup
# (a read-only bind is enough) so its clipboard guard can locate the clients.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-wayland-data.sh NEW_OUTPUT}
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
out=$(realpath -m "$out")
labwc=${LOCACHY_TEST_LABWC:-/usr/bin/labwc}
mkdir -p "$out/proto" "$out/labwc"
for p in xdg-shell primary-selection-unstable-v1 virtual-keyboard-unstable-v1 \
         wlr-virtual-pointer-unstable-v1 ext-data-control-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/proto/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/proto/$p-protocol.c"
done
protos=("$out/proto/xdg-shell-protocol.c" "$out/proto/primary-selection-unstable-v1-protocol.c"
        "$out/proto/virtual-keyboard-unstable-v1-protocol.c" "$out/proto/wlr-virtual-pointer-unstable-v1-protocol.c")
cc=(gcc -O2 -g -Wall -Wextra -Werror -I"$src/base/include" -I"$src/base" -I"$out/proto")
"${cc[@]}" "$src/tools/test-wayland-data.c" "${protos[@]}" -lwayland-client -lxkbcommon -o "$out/wdata"
"${cc[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer "$src/tools/test-wayland-data.c" "${protos[@]}" \
    -lwayland-client -lxkbcommon -o "$out/wdata-san"
"${cc[@]}" "$src/tools/test-clipboard-client.c" "$out/proto/ext-data-control-v1-protocol.c" -lwayland-client -o "$out/client"
# A direct (static, musl-style) build has no Wayland: the stubs compile alone.
gcc -O2 -Wall -Wextra -Werror -I"$src/base/include" -c "$src/base/wayland-data.c" -o "$out/stubs.o"
if nm "$out/stubs.o" | grep -q ' U wl_'; then echo "FAIL stubs reference Wayland"; exit 1; fi
echo "PASS without CONFIG_WAYLAND wayland-data.c needs no Wayland library"

status=0
for b in wdata wdata-san; do
    if "$out/$b" unit >"$out/unit-$b.log" 2>&1 && grep -qx 'ALL PASSED' "$out/unit-$b.log" &&
       ! grep -qE 'Sanitizer|runtime error' "$out/unit-$b.log"; then
        grep '^PASS' "$out/unit-$b.log" | sed "s/^PASS/PASS ($b)/"
    else
        echo "FAIL write-queue unit checks ($b): $out/unit-$b.log"
        status=1
    fi
done

printf '<?xml version="1.0"?>\n<labwc_config/>\n' >"$out/labwc/rc.xml"
export src out labwc
unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mkdir -p /run/user /dev/pts
    mount -t tmpfs tmpfs /run/user
    # foot needs a terminal: a private pty instance, not the host'"'"'s.
    mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts /dev/pts
    [ -e /dev/ptmx ] || ln -s pts/ptmx /dev/ptmx
    install -d -m 0700 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
        "$labwc" -C "$out/labwc" -c "$out/labwc/rc.xml" >"$out/labwc.log" 2>&1 &
    for _ in $(seq 1 60); do [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break; sleep 0.1; done
    if [ -S "$XDG_RUNTIME_DIR/locachy-clipboard-guard.sock" ]; then
        cgroup=$(grep -m1 "^0::/" /proc/self/cgroup || true)
        if [[ -z $cgroup || ! -r /sys/fs/cgroup${cgroup#0::}/cgroup.freeze ]]; then
            printf "FAIL fixture: labwc clipboard guard needs the test cgroup visible under /sys/fs/cgroup\n" >&2
            exit 1
        fi
    fi
    dropped=/tmp/locachy-dropped.$$
    rm -f "$dropped"
    foot -o initial-window-size-pixels=700x500 -- sh -c "stty -icanon -echo; exec cat >$dropped" \
        >"$out/foot.log" 2>&1 &
    sleep 2
    mkfifo "$out/cmd"
    "$out/wdata-san" <"$out/cmd" >"$out/wdata.out" 2>"$out/wdata.err" &
    wdata=$!
    exec 3>"$out/cmd"
    for _ in $(seq 1 100); do grep -q ^ready "$out/wdata.out" && break; sleep 0.05; done
    ask() {
        local before
        before=$(wc -l <"$out/wdata.out")
        printf "%s\n" "$*" >&3
        for _ in $(seq 1 300); do [ "$(wc -l <"$out/wdata.out")" -gt "$before" ] && break; sleep 0.05; done
        tail -n 1 "$out/wdata.out"
    }
    hex() { printf "%s" "$1" | od -An -tx1 | tr -d " \n"; }
    fails=0
    check() { if eval "$1"; then echo "PASS $2"; else echo "FAIL $2 ($3)"; fails=$((fails + 1)); fi; }
    client() { timeout 5 "$out/client" "$@"; }
    start_source() {
        local selection=$1 log=$2 reply
        shift 2
        ask watch-offer "$selection" >/dev/null
        "$out/client" source "$@" 2>"$log" &
        source_pid=$!
        reply=$(ask wait-offer "$selection")
        check "[[ \$reply == \"wait-offer ok=1\" ]]" "the new $selection offer reaches the window" "$reply / $log"
    }

    r=$(grep ^ready "$out/wdata.out" || true)
    check "[[ \$r == *focused=1* ]]" "the test window is mapped and has keyboard focus" "$r"

    r=$(ask key)
    check "[[ \$r == \"key ok=1 focused=1 serial=\"* ]]" "a key press gives the focused window a real keyboard serial" "$r"
    start_source regular "$out/src1.log" foreign
    want=$(hex "foreign 中文 🌍
next line
")
    r=$(ask get)
    check "[[ \$r == \"get status=0 \"*\" sealed=1 data=$want\" ]]" \
        "another client'"'"'s selection is read as a sealed memfd with its exact bytes" "$r"

    reject_changes() {
        local reason=$1 reply
        reply=$(ask "set rejected")
        check "[[ \$reply == \"set status=4 confirmed=0\" ]]" "$reason: set fails without taking ownership" "$reply"
        reply=$(ask get)
        check "[[ \$reply == \"get status=0 \"*\" sealed=1 data=$want\" ]]" \
            "$reason: a rejected set leaves the exact foreign selection readable" "$reply"
        reply=$(ask clear)
        check "[[ \$reply == \"clear status=4\" ]]" "$reason: clear fails" "$reply"
        reply=$(ask get)
        check "[[ \$reply == \"get status=0 \"*\" sealed=1 data=$want\" ]]" \
            "$reason: a rejected clear leaves the exact foreign selection readable" "$reply"
    }
    # The foreign selection was published after the first key serial.
    reject_changes "superseded serial"

    r=$(ask key)
    check "[[ \$r == \"key ok=1 focused=1 serial=\"* ]]" "a later key press gives the focused window a fresh serial" "$r"
    # Mask one backend input at a time. Real Wayland focus and the existing
    # offer stay in place, so the exact bytes can still be checked.
    ask input no-focus >/dev/null
    reject_changes "missing backend focus"
    ask input no-serial >/dev/null
    reject_changes "missing backend serial"
    ask input normal >/dev/null

    r=$(ask "set-get ours 中文 🌍")
    ms=$(sed -n "s/.* ms=\([0-9]*\).*/\1/p" <<<"$r")
    check "[[ \$r == \"set status=0 confirmed=1; get status=0 \"*\" sealed=1 data=$(hex "ours 中文 🌍")\" && \$ms -lt 200 ]]" \
        "a fresh serial commits the selection and reading it immediately does not wait for our own loop" "$r"
    got=$(client read "text/plain;charset=utf-8" || true)
    check "[[ \$got == \"ours 中文 🌍\" ]]" "another client reads our selection" "$got"
    got=$(client read UTF8_STRING || true)
    check "[[ \$got == \"ours 中文 🌍\" ]]" "the UTF8_STRING alias is offered too" "$got"

    ask key >/dev/null
    r=$(ask "set primary prim 中文")
    got=$(client read "text/plain;charset=utf-8" --primary || true)
    check "[[ \$r == \"set status=0 confirmed=1\" && \$got == \"prim 中文\" ]]" "the primary selection is set and read separately" "$r / $got"

    ask key >/dev/null
    r=$(ask "set-get replacement 中文 🌍")
    got=$(client read "text/plain;charset=utf-8" || true)
    check "[[ \$r == \"set status=0 confirmed=1; get status=0 \"*\" sealed=1 data=$(hex "replacement 中文 🌍")\" && \$got == \"replacement 中文 🌍\" ]]" \
        "replacing our own source immediately publishes the new bytes locally and to another client" "$r / $got"
    r=$(ask get primary)
    check "[[ \$r == \"get status=0 \"*\" sealed=1 data=$(hex "prim 中文")\" ]]" \
        "replacing the regular selection does not change our primary selection" "$r"

    start_source primary "$out/src2.log" --primary pfor
    psrc=$source_pid
    primary_want=$(hex "pfor 中文 🌍
next line
")
    r=$(ask get primary)
    check "[[ \$r == \"get status=0 \"*\" sealed=1 data=$primary_want\" ]]" "another client'"'"'s primary selection is read" "$r"
    r=$(ask get)
    check "[[ \$r == *\"data=$(hex "replacement 中文 🌍")\" ]]" "the regular selection is independent of the primary" "$r"

    ask key >/dev/null
    r=$(ask set png 89504e470d0a1a0a000102ff)
    got=$(client read image/png | od -An -tx1 | tr -d " \n" || true)
    check "[[ \$r == *status=0* && \$got == 89504e470d0a1a0a000102ff ]]" "an image/png selection keeps its NUL bytes" "$r / $got"
    got=$(client read "text/plain;charset=utf-8" | wc -c || true)
    check "[[ \$got == 0 ]]" "an image selection offers no text" "$got bytes"
    r=$(ask get)
    check "[[ \$r == \"get status=2 \"* ]]" "our own image is NO_TEXT to a text reader" "$r"

    start_source regular "$out/src3.log" pngsrc
    r=$(ask get png)
    check "[[ \$r == *data=89504e470d0a1a0a000102ff ]]" "another client'"'"'s image/png is read exactly" "$r"

    start_source regular "$out/src4.log" --hang stall
    hang=$source_pid
    r=$(ask get)
    ms=$(sed -n "s/.* ms=\([0-9]*\).*/\1/p" <<<"$r")
    check "[[ \$r == \"get status=3 \"* && \$ms -ge 2000 && \$ms -lt 4000 ]]" \
        "a source that never writes ends as BUSY at the 2.5 s deadline" "$r"
    kill "$hang" 2>/dev/null || true

    ask key >/dev/null
    r=$(ask "set again")
    check "[[ \$r == \"set status=0 confirmed=1\" ]]" "a new selection is owned before clearing it" "$r"
    ask key >/dev/null
    r=$(ask clear)
    local_get=$(ask get)
    rc=0; client read "text/plain;charset=utf-8" >/dev/null || rc=$?
    check "[[ \$r == \"clear status=0\" && \$local_get == \"get status=1 size=0 \"* && \$rc == 3 ]]" \
        "clearing leaves no local or external selection" "$r / $local_get / rc=$rc"
    r=$(ask clear)
    local_get=$(ask get)
    rc=0; client read "text/plain;charset=utf-8" >/dev/null || rc=$?
    check "[[ \$r == \"clear status=0\" && \$local_get == \"get status=1 size=0 \"* && \$rc == 3 ]]" \
        "clearing an already empty selection is idempotent" "$r / $local_get / rc=$rc"
    r=$(ask get primary)
    check "[[ \$r == \"get status=0 \"*\" sealed=1 data=$primary_want\" ]]" \
        "clearing the regular selection leaves the exact foreign primary bytes" "$r"
    kill "$psrc" 2>/dev/null || true

    r=$(ask where)
    ox=$(sed -n "s/.* x=\(-*[0-9]*\).*/\1/p" <<<"$r"); oy=$(sed -n "s/.* y=\(-*[0-9]*\).*/\1/p" <<<"$r")
    check "[[ \$r == *entered=1* ]]" "the test window is under the pointer at the output centre" "$r"
    cx=$((ox + 100)); cy=$((oy + 75))
    : >/tmp/locachy-dragged.txt
    r=$(ask drag $cx $cy $((ox - 150)) $cy /tmp/locachy-dragged.txt)
    sleep 0.5
    got=$(cat "$dropped" 2>/dev/null || true)
    check "[[ \$r == *\"started=0 active=0 finished=1\"* && \$got == *locachy-dragged.txt* ]]" \
        "a text/uri-list drag dropped on foot is read by foot (real press serial)" "$r / $got"
    r=$(ask drag $cx $cy $((cx + 50)) $cy /tmp/locachy-dragged.txt)
    check "[[ \$r == *\"started=0 active=0 finished=0\"* ]]" \
        "a drop on the window itself (not a drop target) is cancelled, nothing copied" "$r"

    ask quit >/dev/null
    exec 3>&-
    wait "$wdata" || fails=$((fails + 1))
    if grep -qE "Sanitizer|runtime error" "$out/wdata.err"; then echo "FAIL sanitizer report: $out/wdata.err"; fails=$((fails + 1)); fi
    rm -f "$dropped" /tmp/locachy-dragged.txt
    exit "$fails"
' || status=1
exit "$status"
