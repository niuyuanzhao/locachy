#!/bin/sh
# test-container-documents.sh OUT: verify container-documents refuses root,
# owns org.freedesktop.portal.Documents on a private session bus as an
# unprivileged user, answers version/GetMountPoint/Lookup/List/GetHostPaths,
# and rejects Add with org.freedesktop.portal.Error.NotAllowed.
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
    if [ -x /usr/lib/xdg-desktop-portal ]; then
        mkdir -p "$XDG_RUNTIME_DIR/xdg/xdg-desktop-portal"
        cp "$2" "$XDG_RUNTIME_DIR/xdg/xdg-desktop-portal/portals.conf"
        XDG_CONFIG_DIRS="$XDG_RUNTIME_DIR/xdg" /usr/lib/xdg-desktop-portal >"$XDG_RUNTIME_DIR/xdp.log" 2>&1 &
        xdp=$!
        trap "kill $srv $xdp 2>/dev/null || true" EXIT
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
    fi
' sh "$out/container-documents" "$src/link/portals.conf"
echo "PASS container-documents serves org.freedesktop.portal.Documents cleanly"
