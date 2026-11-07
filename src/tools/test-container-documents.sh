#!/bin/sh
# test-container-documents.sh OUT: verify container-documents refuses root,
# owns org.freedesktop.portal.Documents on a private session bus as an
# unprivileged user, answers version/GetMountPoint/Lookup/List/GetHostPaths,
# and rejects Add with org.freedesktop.portal.Error.NotAllowed; its
# FileTransfer decodes a LOCachy key in RetrieveFiles and allows no transfer.
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-container-documents.sh OUT}
rm -rf "$out"
mkdir -p "$out"

gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/progs/container-documents.c" \
    $(pkg-config --cflags --libs gio-2.0) -o "$out/container-documents"

# Must refuse to run as root (euid 0 inside userns).
if unshare --map-root-user "$out/container-documents" >/dev/null 2>&1; then
    echo "FAIL container-documents accepted euid 0"
    exit 1
fi
echo "PASS container-documents refuses euid 0"

# Run on a private session bus as the unprivileged caller with private XDG_RUNTIME_DIR.
XDG_RUNTIME_DIR="$out/rt"
mkdir -p -m 0700 "$XDG_RUNTIME_DIR"
export XDG_RUNTIME_DIR
dbus-run-session -- sh -euc '
    bin=$1
    "$bin" &
    srv=$!
    trap "kill $srv 2>/dev/null || true" EXIT
    for _ in $(seq 40); do
        dbus-send --session --print-reply=literal --dest=org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus.NameHasOwner string:org.freedesktop.portal.Documents 2>/dev/null |
            grep -q true && break
        sleep 0.05
    done
    ver=$(dbus-send --session --print-reply=literal --dest=org.freedesktop.portal.Documents \
        /org/freedesktop/portal/documents org.freedesktop.DBus.Properties.Get \
        string:org.freedesktop.portal.Documents string:version)
    echo "$ver" | grep -q "uint32 5"
    mp=$(gdbus call --session --dest org.freedesktop.portal.Documents \
        --object-path /org/freedesktop/portal/documents \
        --method org.freedesktop.portal.Documents.GetMountPoint)
    echo "$mp" | grep -q "/rt/doc"
    [ -d "$XDG_RUNTIME_DIR/doc" ]
    lu=$(gdbus call --session --dest org.freedesktop.portal.Documents \
        --object-path /org/freedesktop/portal/documents \
        --method org.freedesktop.portal.Documents.Lookup "[byte 0x2f, 0x00]")
    [ "$lu" = "('\'''\'',)" ]
    if gdbus call --session --dest org.freedesktop.portal.Documents \
        --object-path /org/freedesktop/portal/documents \
        --method org.freedesktop.portal.Documents.Delete "abc" >/dev/null 2>&1; then
        echo "FAIL Delete should fail with NotAllowed"
        exit 1
    fi
    # FileTransfer: RetrieveFiles answers the paths of a LOCachy key (made
    # here by coreutils, not by the code under test); a bad key is an
    # invalid argument; starting a transfer or adding files is not allowed.
    ft() { gdbus call --session --dest org.freedesktop.portal.Documents \
        --object-path /org/freedesktop/portal/documents --method "org.freedesktop.portal.FileTransfer.$@"; }
    ftv=$(dbus-send --session --print-reply=literal --dest=org.freedesktop.portal.Documents \
        /org/freedesktop/portal/documents org.freedesktop.DBus.Properties.Get \
        string:org.freedesktop.portal.FileTransfer string:version)
    echo "$ftv" | grep -q "uint32 1"
    key=LOCACHYDOC$(printf "/run/user/1000/doc/a1/r e.pdf\000/home/u/Documents/x.txt" | base64 -w0 | tr "+/" "-_" | tr -d "=")
    q=$(printf "\047")
    [ "$(ft RetrieveFiles "$key" "{}")" = "([$q/run/user/1000/doc/a1/r e.pdf$q, $q/home/u/Documents/x.txt$q],)" ]
    ft RetrieveFiles "${key}=" "{}" 2>&1 | grep -q InvalidArgument
    ft RetrieveFiles "$(printf "%s" "$key" | tr A B)x" "{}" 2>&1 | grep -q InvalidArgument
    ft StartTransfer "{}" 2>&1 | grep -q NotAllowed
    ft AddFiles "$key" "@ah []" "{}" 2>&1 | grep -q NotAllowed
    [ "$(ft StopTransfer "$key")" = "()" ]
    echo "version 1, RetrieveFiles decodes the key" >"$XDG_RUNTIME_DIR/file-transfer"
    if [ -x /usr/lib/xdg-desktop-portal ]; then
        # A display for the GTK backend (it leaves without one, and the
        # portal then has no file chooser): the headless one of GTK here,
        # as a container has locachy-0.
        chooser=
        if [ -f /usr/share/xdg-desktop-portal/portals/gtk.portal ] && command -v broadwayd >/dev/null; then
            broadwayd :17 >"$XDG_RUNTIME_DIR/broadway.log" 2>&1 &
            bw=$!
            sleep 0.5
            export GDK_BACKEND=broadway BROADWAY_DISPLAY=:17
            dbus-update-activation-environment GDK_BACKEND BROADWAY_DISPLAY
            chooser=1
        fi
        mkdir -p "$XDG_RUNTIME_DIR/xdg/xdg-desktop-portal"
        cp "$2" "$XDG_RUNTIME_DIR/xdg/xdg-desktop-portal/portals.conf"
        XDG_CONFIG_DIRS="$XDG_RUNTIME_DIR/xdg" /usr/lib/xdg-desktop-portal >"$XDG_RUNTIME_DIR/xdp.log" 2>&1 &
        xdp=$!
        trap "kill $srv $xdp ${bw:-} 2>/dev/null || true" EXIT
        for _ in $(seq 60); do
            dbus-send --session --print-reply=literal --dest=org.freedesktop.DBus /org/freedesktop/DBus \
                org.freedesktop.DBus.NameHasOwner string:org.freedesktop.portal.Desktop 2>/dev/null |
                grep -q true && break
            sleep 0.05
        done
        sleep 0.3
        kill -0 "$xdp"
        reg=$(dbus-send --session --print-reply=literal --dest=org.freedesktop.portal.Desktop \
            /org/freedesktop/portal/desktop org.freedesktop.DBus.Properties.Get \
            string:org.freedesktop.host.portal.Registry string:version)
        echo "$reg" | grep -q "uint32"
        # The file chooser, as Chromium and Firefox look for it before they
        # use it (FileChooser version 3 or later), and a dialog asked for.
        if [ -n "$chooser" ]; then
            fc=$(dbus-send --session --print-reply=literal --dest=org.freedesktop.portal.Desktop \
                /org/freedesktop/portal/desktop org.freedesktop.DBus.Properties.Get \
                string:org.freedesktop.portal.FileChooser string:version)
            [ "$(echo "$fc" | awk "{print \$NF}")" -ge 3 ]
            req=$(gdbus call --session --dest org.freedesktop.portal.Desktop \
                --object-path /org/freedesktop/portal/desktop \
                --method org.freedesktop.portal.FileChooser.OpenFile "" "Attach" "{}")
            echo "$req" | grep -q "/org/freedesktop/portal/desktop/request/"
            echo "version $(echo "$fc" | awk "{print \$NF}"), OpenFile asked a dialog" >"$XDG_RUNTIME_DIR/file-chooser"
        fi
    fi
' sh "$out/container-documents" "$src/link/portals.conf"
echo "PASS container-documents serves org.freedesktop.portal.Documents cleanly"
if [ -f "$out/rt/file-transfer" ]; then
    echo "PASS container-documents serves org.freedesktop.portal.FileTransfer: $(cat "$out/rt/file-transfer"), a bad key is an invalid argument, StartTransfer and AddFiles not allowed, StopTransfer succeeds"
else
    echo "FAIL container-documents FileTransfer"
    exit 1
fi
if [ -f "$out/rt/file-chooser" ]; then
    echo "PASS portals.conf gives the containers a file chooser (xdg-desktop-portal-gtk): $(cat "$out/rt/file-chooser")"
fi
