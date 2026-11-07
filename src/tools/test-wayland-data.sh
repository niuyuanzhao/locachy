#!/bin/bash
# test-wayland-data.sh NEW_OUTPUT: base/wayland-data.c in a real window of a
# headless labwc. Its clipboard and primary selection meet a data-control
# client; its text/uri-list drag is dropped on foot, a separate program. Run
# as root in the development root: private mount/PID namespace, no host
# display, clipboard or input device. Also the write queue's unit checks.
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
    source_ready() { for _ in $(seq 1 60); do grep -q ready "$1" && break; sleep 0.05; done; }
    client() { timeout 5 "$out/client" "$@"; }

    r=$(grep ^ready "$out/wdata.out" || true)
    check "[[ \$r == *focused=1* ]]" "the test window is mapped and has keyboard focus" "$r"

    client source foreign 2>"$out/src1.log" &
    source_ready "$out/src1.log"
    r=$(ask get)
    want=$(hex "foreign 中文 🌍
next line
")
    check "[[ \$r == \"get status=0 \"*\" sealed=1 data=$want\" ]]" \
        "another client'"'"'s selection is read as a sealed memfd with its exact bytes" "$r"

    r=$(ask key)
    check "[[ \$r == *ok=1* ]]" "a key press gives the window a real keyboard serial" "$r"
    r=$(ask "set ours 中文 🌍")
    check "[[ \$r == \"set status=0 confirmed=1\" ]]" "set_selection with that serial is accepted and echoed back" "$r"
    got=$(client read "text/plain;charset=utf-8" || true)
    check "[[ \$got == \"ours 中文 🌍\" ]]" "another client reads our selection" "$got"
    got=$(client read UTF8_STRING || true)
    check "[[ \$got == \"ours 中文 🌍\" ]]" "the UTF8_STRING alias is offered too" "$got"
    r=$(ask get)
    ms=$(sed -n "s/.* ms=\([0-9]*\).*/\1/p" <<<"$r")
    check "[[ \$r == *\"data=$(hex "ours 中文 🌍")\" && \$ms -lt 200 ]]" \
        "reading our own selection does not wait for our own loop" "$r"

    r=$(ask "set primary prim 中文")
    got=$(client read "text/plain;charset=utf-8" --primary || true)
    check "[[ \$r == *status=0* && \$got == \"prim 中文\" ]]" "the primary selection is set and read separately" "$r / $got"
    client source --primary pfor 2>"$out/src2.log" &
    psrc=$!
    source_ready "$out/src2.log"
    r=$(ask get primary)
    check "[[ \$r == *\"data=$(hex "pfor 中文 🌍")\"* ]]" "another client'"'"'s primary selection is read" "$r"
    kill "$psrc" 2>/dev/null || true
    r=$(ask get)
    check "[[ \$r == *\"data=$(hex "ours 中文 🌍")\" ]]" "the regular selection is independent of the primary" "$r"

    r=$(ask set png 89504e470d0a1a0a000102ff)
    got=$(client read image/png | od -An -tx1 | tr -d " \n" || true)
    check "[[ \$r == *status=0* && \$got == 89504e470d0a1a0a000102ff ]]" "an image/png selection keeps its NUL bytes" "$r / $got"
    got=$(client read "text/plain;charset=utf-8" | wc -c || true)
    check "[[ \$got == 0 ]]" "an image selection offers no text" "$got bytes"
    r=$(ask get)
    check "[[ \$r == \"get status=2 \"* ]]" "our own image is NO_TEXT to a text reader" "$r"

    client source pngsrc 2>"$out/src3.log" &
    source_ready "$out/src3.log"
    r=$(ask get png)
    check "[[ \$r == *data=89504e470d0a1a0a000102ff ]]" "another client'"'"'s image/png is read exactly" "$r"

    client source --hang stall 2>"$out/src4.log" &
    hang=$!
    source_ready "$out/src4.log"
    r=$(ask get)
    ms=$(sed -n "s/.* ms=\([0-9]*\).*/\1/p" <<<"$r")
    check "[[ \$r == \"get status=3 \"* && \$ms -ge 2000 && \$ms -lt 4000 ]]" \
        "a source that never writes ends as BUSY at the 2.5 s deadline" "$r"
    kill "$hang" 2>/dev/null || true

    ask key >/dev/null
    ask "set again" >/dev/null
    r=$(ask clear)
    rc=0; client read "text/plain;charset=utf-8" >/dev/null || rc=$?
    check "[[ \$r == \"clear status=0\" && \$rc == 3 ]]" "clearing leaves no selection" "$r rc=$rc"

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
