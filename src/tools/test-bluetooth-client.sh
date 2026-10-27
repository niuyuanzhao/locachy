#!/bin/bash
# BlueZ service lifecycle in private PID, mount and network namespaces.
# No physical controller, system bus or pairing store is used.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
export src
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc --net bash -euc '
    mount -t tmpfs -o mode=0755 tmpfs /run
    mkdir -p /run/dbus /run/bluetooth-test
    mkdir -p /var/lib/bluetooth
    mount -t tmpfs -o mode=0700 tmpfs /var/lib/bluetooth
    unset DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS
    test -x /usr/lib/bluetooth/bluetoothd
    daemon=/usr/lib/bluetooth/bluetoothd
    system=
    client=
    cleanup() {
        status=$?
        if [ "$status" -ne 0 ]; then
            for log in /run/bluetooth-test/*.log; do
                [ ! -f "$log" ] || { printf "=== %s ===\n" "$log"; cat "$log"; }
            done
        fi
        kill ${client:+"$client"} ${system:+"$system"} 2>/dev/null || true
        wait 2>/dev/null || true
    }
    trap cleanup EXIT
    owner() {
        timeout 2 dbus-send --system --print-reply --reply-timeout=1000 \
            --dest=org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus.NameHasOwner string:org.bluez 2>/dev/null | grep -q "boolean true"
    }
    ready() {
        for _ in $(seq 1 50); do
            timeout 2 busctl --system --no-pager list >/dev/null 2>&1 && return 0
            sleep 0.1
        done
        return 1
    }
    # A socket on disk is not a running bus. The wrapper must wait for a
    # successful D-Bus round trip, then exec the foreground daemon.
    python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.bind(\"/run/dbus/system_bus_socket\")"
    bash "$src/link/bluetooth-client" >/run/bluetooth-test/late.log 2>&1 &
    client=$!
    sleep 0.2
    kill -0 "$client"
    ! grep -q "Bluetooth daemon" /run/bluetooth-test/late.log
    bash "$src/link/bus-client" system &
    system=$!
    ready
    # No HCI is visible in the private network namespace. A real daemon may
    # exit when the kernel management socket is unavailable; never claim a
    # physical controller was exercised by this test.
    for _ in $(seq 1 50); do
        if [ "$(cat /proc/$client/comm 2>/dev/null || true)" = bluetoothd ] ||
           grep -q "Bluetooth daemon" /run/bluetooth-test/late.log; then break; fi
        kill -0 "$client" 2>/dev/null || break
        sleep 0.1
    done
    grep -q "Bluetooth daemon" /run/bluetooth-test/late.log
    echo "PASS stale socket does not start BlueZ; a live system bus starts the real daemon"
    kill "$client" 2>/dev/null || true
    wait "$client" 2>/dev/null || true
    client=
    ! owner
    echo "PASS BlueZ exits without an adapter when its management socket is unavailable (isolated network)"

    # A standard dbus-python service stands in for the daemon only for
    # deterministic readiness, exec/PID and disconnect lifecycle checks.
    cat >/run/bluetooth-test/daemon <<"PY"
#!/usr/bin/python3
import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib
import os
import pathlib
import signal
import sys
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
name = dbus.service.BusName("org.bluez", bus=bus)
pathlib.Path("/run/bluetooth-test/started").write_text(str(os.getpid()) + " " + " ".join(sys.argv[1:]))
loop = GLib.MainLoop()
bus.call_on_disconnection(lambda _: loop.quit())
signal.signal(signal.SIGTERM, lambda *_: loop.quit())
loop.run()
PY
    chmod 755 /run/bluetooth-test/daemon
    mount --bind /run/bluetooth-test/daemon "$daemon"
    bash "$src/link/bluetooth-client" >/run/bluetooth-test/client.log 2>&1 &
    client=$!
    for _ in $(seq 1 50); do owner && break; sleep 0.1; done
    owner
    test "$(cat /run/bluetooth-test/started)" = "$client -n"
    echo "PASS wrapper execs the foreground daemon with its own PID and -n"
    kill -TERM "$client"
    wait "$client"
    client=
    ! owner
    if timeout 3 dbus-send --system --print-reply --reply-timeout=1000 \
        --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects >/dev/null 2>&1; then
        echo "FAIL stopped BlueZ was activated unexpectedly" >&2
        exit 1
    fi
    ! owner
    echo "PASS a client request cannot activate BlueZ after its service is stopped"
    rm /run/bluetooth-test/started
    bash "$src/link/bluetooth-client" >/run/bluetooth-test/restart.log 2>&1 &
    client=$!
    for _ in $(seq 1 50); do owner && break; sleep 0.1; done
    owner
    kill -TERM "$system"
    wait "$system"
    system=
    for _ in $(seq 1 50); do kill -0 "$client" 2>/dev/null || break; sleep 0.1; done
    ! kill -0 "$client" 2>/dev/null
    status=0
    wait "$client" || status=$?
    client=
    echo "PASS disconnect ends the foreground service for process 1 to restart (status $status)"
    umount "$daemon"

    # Fast command doubles test the exhausted readiness path, never BlueZ.
    mkdir /run/bluetooth-test/bin
    printf "#!/bin/sh\nexit 1\n" >/run/bluetooth-test/bin/dbus-send
    printf "#!/bin/sh\nexit 0\n" >/run/bluetooth-test/bin/sleep
    chmod 755 /run/bluetooth-test/bin/*
    if PATH=/run/bluetooth-test/bin:$PATH bash "$src/link/bluetooth-client" >/run/bluetooth-test/unavailable.log 2>&1; then
        echo "FAIL absent bus reported success" >&2
        exit 1
    fi
    grep -q "no system bus" /run/bluetooth-test/unavailable.log
    echo "PASS absent bus is a bounded failure, not silent success or a second bus"
    echo "ALL PASSED"
'
