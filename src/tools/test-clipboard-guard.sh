#!/bin/bash
# Real compositor gate in private mount/PID namespaces and nested cgroups.
# LABWC must name a candidate with the LOCachy guard, never the live display.
# Run test-clipboard.sh first to build the clipboard test binary.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
labwc=${1:?usage: test-clipboard-guard.sh CANDIDATE_LABWC}
out=$(mktemp -d /var/tmp/locachy-clipboard-guard-test.XXXXXX)
clip=${LOCACHY_TEST_CLIPBOARD:-/usr/local/lib/locachy/clipboard}
mkdir -p "$out"
group=$(mktemp -d /sys/fs/cgroup/locachy-guard-test.XXXXXX)
cleanup() {
    for name in app_source app_abort app_other; do
        if [ -d "$group/$name" ]; then
            printf '0\n' >"$group/$name/cgroup.freeze"
            printf '1\n' >"$group/$name/cgroup.kill"
            rmdir "$group/$name" 2>/dev/null || true
        fi
    done
    rmdir "$group" 2>/dev/null || true
}
trap cleanup EXIT
mkdir "$group/app_source" "$group/app_abort" "$group/app_other"
for p in xdg-shell virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1 primary-selection-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/$p-protocol.c"
done
gcc -O2 -Wall -Wextra -Werror -I"$out" "$src/tools/test-clipboard-ordinary.c" \
    "$out/xdg-shell-protocol.c" "$out/primary-selection-unstable-v1-protocol.c" -lwayland-client -o "$out/ordinary"
gcc -O2 -Wall -Wextra -Werror -I"$out" "$src/tools/test-seat.c" \
    "$out/virtual-keyboard-unstable-v1-protocol.c" "$out/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -lwayland-client -lxkbcommon -o "$out/seat"
cat >"$out/transaction.py" <<'PY'
import array, os, socket, sys, time
runtime = os.environ['XDG_RUNTIME_DIR']
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.settimeout(4)
s.connect(runtime + '/locachy-clipboard-guard.sock')
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY)
s.sendmsg([b'begin'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
os.close(fd)
reply = s.recv(128)
if reply != b'ready':
    raise RuntimeError(reply)
open(sys.argv[2], 'w').write('ready')
if len(sys.argv) == 4 and sys.argv[3] == 'abort':
    s.close()
else:
    if len(sys.argv) == 4 and sys.argv[3] == 'freeze-disconnect':
        with open(sys.argv[1] + '/cgroup.freeze', 'w') as f:
            f.write('1')
        s.close()
    elif len(sys.argv) == 4 and sys.argv[3] in ('timeout', 'late-commit'):
        time.sleep(7)
        if sys.argv[3] == 'late-commit':
            try:
                s.send(b'commit')
                reply = s.recv(128)
                if reply == b'committed':
                    raise RuntimeError('expired transaction committed')
            except (BrokenPipeError, ConnectionResetError):
                pass
            with open(sys.argv[1] + '/cgroup.freeze') as f:
                if f.read().strip() != '0':
                    raise RuntimeError('expired helper froze the source')
    else:
        while not os.path.exists(sys.argv[2] + '.finish'):
            time.sleep(.01)
        s.send(b'abort')
        if s.recv(128) != b'aborted':
            raise RuntimeError('abort failed')
PY
export src out group labwc clip
unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs tmpfs /run/user
    mount -t tmpfs tmpfs /run/locachy
    install -d -m 0711 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
    export LOCACHY_CLIPBOARD_SOCKET=/run/locachy/clipboard.sock
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
        "$labwc" -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >"$out/labwc.log" 2>&1 &
    comp=$!
    # The guard socket may come before the display socket the helpers use.
    for _ in $(seq 1 80); do
        [ -S "$XDG_RUNTIME_DIR/locachy-clipboard-guard.sock" ] && [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break
        sleep 0.1
    done
    test -S "$XDG_RUNTIME_DIR/locachy-clipboard-guard.sock"
    test -S "$XDG_RUNTIME_DIR/wayland-0"
    "$out/seat" >"$out/seat.log" 2>&1 &
    input=$!
    /usr/local/lib/locachy/labwc-control >"$out/control.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
    "$clip" >"$out/clipboard.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S "$LOCACHY_CLIPBOARD_SOCKET" ] && break; sleep 0.1; done
    (echo 0 >"$group/app_source/cgroup.procs"; WAYLAND_DISPLAY=locachy-0 exec "$out/ordinary" source source-original) \
        >"$out/source.log" 2>"$out/source.err" &
    source=$!
    for _ in $(seq 1 50); do grep -q activated "$out/source.log" && break; sleep 0.1; done
    kill -USR1 "$input"
    for _ in $(seq 1 50); do "$clip" get 2>/dev/null | grep -qx source-original && break; sleep 0.1; done
    "$clip" get | grep -qx source-original
    "$clip" sync >/dev/null

    rm -f "$out/ready" "$out/ready.finish"
    python3 "$out/transaction.py" "$group/app_source" "$out/ready" >"$out/transaction.log" 2>&1 &
    transaction=$!
    for _ in $(seq 1 50); do [ -e "$out/ready" ] && break; sleep 0.1; done
    test -e "$out/ready"
    printf retained-by-host | "$clip" set
    printf primary-retained-by-host | "$clip" set --primary
    "$clip" sync >/dev/null
    # The source has a valid serial, but its delayed request arrives after
    # begin: it must not become selection owner even though it still runs.
    kill -USR1 "$source"
    for _ in $(seq 1 50); do grep -q "ordinary copy repeated" "$out/source.log" && break; sleep 0.1; done
    sleep 0.2
    "$clip" sync >/dev/null
    "$clip" get | grep -qx retained-by-host
    "$clip" get --primary | grep -qx primary-retained-by-host
    echo "PASS delayed ordinary and primary selection after begin are rejected at compositor dispatch"
    # A second connection created after begin is in the same cgroup and must
    # hit the same gate; a different cgroup may still change the selection.
    (echo 0 >"$group/app_source/cgroup.procs"; WAYLAND_DISPLAY=locachy-0 exec "$out/ordinary" source second-connection) \
        >"$out/second.log" 2>"$out/second.err" &
    second=$!
    for _ in $(seq 1 50); do grep -q activated "$out/second.log" && break; sleep 0.1; done
    kill -USR1 "$input"
    for _ in $(seq 1 50); do grep -q "ordinary copy cancelled" "$out/second.log" && break; sleep 0.1; done
    grep -q "ordinary copy cancelled" "$out/second.log"
    "$clip" get | grep -qx retained-by-host
    kill -TERM "$second"
    wait "$second" 2>/dev/null || true
    (echo 0 >"$group/app_other/cgroup.procs"; WAYLAND_DISPLAY=locachy-0 exec "$out/ordinary" source unrelated-container) \
        >"$out/other.log" 2>"$out/other.err" &
    other=$!
    for _ in $(seq 1 50); do grep -q activated "$out/other.log" && break; sleep 0.1; done
    kill -USR1 "$input"
    for _ in $(seq 1 50); do "$clip" get 2>/dev/null | grep -qx unrelated-container && break; sleep 0.1; done
    "$clip" get | grep -qx unrelated-container
    kill -TERM "$other"
    wait "$other" 2>/dev/null || true
    echo "PASS all connections in the gated cgroup are covered; other cgroups still copy"
    touch "$out/ready.finish"
    wait "$transaction"
    serials=$(grep -c "serial received" "$out/source.log")
    kill -USR1 "$input"
    for _ in $(seq 1 50); do [ "$(grep -c "serial received" "$out/source.log")" -gt "$serials" ] && break; sleep 0.1; done
    kill -USR1 "$source"
    for _ in $(seq 1 50); do "$clip" get 2>/dev/null | grep -qx source-original && break; sleep 0.1; done
    "$clip" get | grep -qx source-original
    "$clip" get --primary | grep -qx source-original
    echo "PASS aborted transaction removes the gate without freezing the source"

    "$clip" freeze "$group/app_source"
    grep -qx "frozen 1" "$group/app_source/cgroup.events"
    # cgroup.events is not a normal readable event source: the idle
    # compositor must not spin for the entire duration of a frozen app.
    before=$(awk "{print \$14 + \$15}" /proc/$comp/stat)
    sleep 0.5
    after=$(awk "{print \$14 + \$15}" /proc/$comp/stat)
    test $((after - before)) -lt 10
    echo "PASS committed freeze does not make the compositor busy-loop"
    printf after-freeze | "$clip" set
    "$clip" get | grep -qx after-freeze
    printf 0 >"$group/app_source/cgroup.freeze"
    serials=$(grep -c "serial received" "$out/source.log")
    kill -USR1 "$input"
    for _ in $(seq 1 50); do [ "$(grep -c "serial received" "$out/source.log")" -gt "$serials" ] && break; sleep 0.1; done
    kill -USR1 "$source"
    for _ in $(seq 1 50); do "$clip" get 2>/dev/null | grep -qx source-original && break; sleep 0.1; done
    "$clip" get | grep -qx source-original
    echo "PASS commit freezes after capture; actual thaw permits the next copy"
    kill -TERM "$source"
    wait "$source" 2>/dev/null || true
    rm -f "$out/disconnect"
    python3 "$out/transaction.py" "$group/app_abort" "$out/disconnect" freeze-disconnect
    for _ in $(seq 1 50); do [ "$(cat "$group/app_abort/cgroup.freeze")" = 0 ] && break; sleep 0.1; done
    test "$(cat "$group/app_abort/cgroup.freeze")" = 0
    echo "PASS helper disconnect after freeze but before commit rolls the cgroup back"
    rm -f "$out/timeout"
    python3 "$out/transaction.py" "$group/app_abort" "$out/timeout" timeout
    python3 "$out/transaction.py" "$group/app_abort" "$out/reopened" abort
    echo "PASS abandoned transaction expires and a fresh transaction can begin"
    python3 "$out/transaction.py" "$group/app_abort" "$out/late" late-commit
    test "$(cat "$group/app_abort/cgroup.freeze")" = 0
    echo "PASS late commit after deadline cannot freeze outside a live gate"
    (echo 0 >"$group/app_source/cgroup.procs"; CLIPBOARD_TEST_DELAY_US=500000 WAYLAND_DISPLAY=locachy-0 \
        exec "$out/ordinary" source slow-before-freeze) >"$out/slow.log" 2>"$out/slow.err" &
    slow=$!
    for _ in $(seq 1 50); do grep -q activated "$out/slow.log" && break; sleep 0.1; done
    kill -USR1 "$input"
    for _ in $(seq 1 50); do grep -q "ordinary copy published" "$out/slow.log" && break; sleep 0.05; done
    "$clip" freeze "$group/app_source"
    grep -qx "frozen 1" "$group/app_source/cgroup.events"
    "$clip" get | grep -qx slow-before-freeze
    "$clip" get --primary | grep -qx slow-before-freeze
    printf 0 >"$group/app_source/cgroup.freeze"
    kill -TERM "$slow"
    wait "$slow" 2>/dev/null || true
    echo "PASS accepted slow source finishes both transfers before the compositor freezes it"
    echo "ALL PASSED"
'
