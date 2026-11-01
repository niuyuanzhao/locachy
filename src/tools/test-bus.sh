#!/bin/bash
# Host buses in private mount and PID namespaces: no host socket or daemon
# is touched. Both run in the foreground, independently; after SIGKILL a
# bus can replace its stale socket, without restarting the other one.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
export src
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mount -t tmpfs -o mode=0755 tmpfs /run/user
    mkdir -m 0711 /run/user/0
    mount -t tmpfs -o mode=0755 tmpfs /run/dbus
    export XDG_RUNTIME_DIR=/run/user/0
    unset DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
    session=
    system=
    monitor=
    cleanup() {
        kill ${monitor:+"$monitor"} ${session:+"$session"} ${system:+"$system"} 2>/dev/null || true
        wait 2>/dev/null || true
    }
    trap cleanup EXIT
    ready() {
        local scope=$1
        for _ in $(seq 1 50); do
            if timeout 1 busctl --"$scope" --no-pager list >/dev/null 2>&1; then
                return 0
            fi
            sleep 0.1
        done
        return 1
    }
    bash "$src/link/bus-client" session &
    session=$!
    bash "$src/link/bus-client" system &
    system=$!
    ready user && ready system
    test "$(cat /proc/$session/comm)" = dbus-daemon
    test "$(cat /proc/$system/comm)" = dbus-daemon
    echo "PASS both buses stay in the foreground"
    dbus-monitor --address "unix:path=$XDG_RUNTIME_DIR/bus" \
        "type=signal,interface=org.freedesktop.DBus.Local,member=Disconnected" \
        >"$XDG_RUNTIME_DIR/monitor.log" 2>&1 &
    monitor=$!
    for _ in $(seq 1 50); do
        grep -q NameAcquired "$XDG_RUNTIME_DIR/monitor.log" && break
        kill -0 "$monitor"
        sleep 0.1
    done
    grep -q NameAcquired "$XDG_RUNTIME_DIR/monitor.log"
    kill -KILL "$session"
    wait "$session" 2>/dev/null || true
    for _ in $(seq 1 50); do
        kill -0 "$monitor" 2>/dev/null || break
        sleep 0.1
    done
    ! kill -0 "$monitor" 2>/dev/null
    wait "$monitor"
    monitor=
    echo "PASS the audio disconnect monitor exits when its bus disappears"
    test -S "$XDG_RUNTIME_DIR/bus"
    kill -0 "$system"
    bash "$src/link/bus-client" session &
    session=$!
    ready user && ready system
    echo "PASS session bus replaces its stale socket; system bus stays alive"
    kill -KILL "$system"
    wait "$system" 2>/dev/null || true
    test -S /run/dbus/system_bus_socket
    kill -0 "$session"
    bash "$src/link/bus-client" system &
    system=$!
    ready user && ready system
    echo "PASS system bus replaces its stale socket; session bus stays alive"
    kill -TERM "$session" "$system"
    wait "$session"
    wait "$system"
    session=
    system=
    echo "PASS both buses end on SIGTERM"
    echo "ALL PASSED"
'
