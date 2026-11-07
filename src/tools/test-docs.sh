#!/bin/sh
# test-docs.sh NEW_OUTPUT: the document service's pieces without a device
# (tools/DOCUMENTS.md). docs-proto.h: the FileTransfer key, the containers'
# app ids, the peers. The server (locachy-docs --user) against a stand-in
# document portal on a private bus, as root of a user namespace with a
# private /run (a user of its own, uid 1000, for the peer check): begin,
# end, drop, view, all or nothing, a connection that closes, a container
# whose cgroup empties. The real portal: test-docs-portal.sh.
set -eu
[ "$#" = 1 ] || { echo "usage: test-docs.sh NEW_OUTPUT" >&2; exit 2; }
out=$(realpath -m "$1")
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
mkdir -p "$out"
src=$(cd "$(dirname "$0")/.." && pwd)
run=/run/test-docs
defs="-DDOCS_DIR=\"$run/t/docs\" -DCGROUP_ROOT=\"$run/t/cgroup\""
cc="gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE $defs"
# shellcheck disable=SC2086
$cc "$src/progs/locachy-docs.c" $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/locachy-docs"
# shellcheck disable=SC2086
$cc "$src/tools/test-docs.c" $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/test-docs"
# shellcheck disable=SC2086
$cc -fsanitize=address,undefined -fno-omit-frame-pointer "$src/progs/locachy-docs.c" \
    $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/locachy-docs-san"

status=0
if "$out/test-docs" unit >"$out/unit.log" 2>&1 && grep -qx 'ALL PASSED' "$out/unit.log"; then
    grep '^PASS' "$out/unit.log"
else
    grep '^FAIL' "$out/unit.log" || true
    echo "FAIL docs-proto.h unit checks: $out/unit.log"
    status=1
fi
for server in locachy-docs locachy-docs-san; do
    unshare --map-auto --map-root-user --mount sh -c '
        mount -t tmpfs -o mode=0755 tmpfs /run || exit 9
        mkdir -p /run/test-docs/t
        install -m 755 "$1" /run/test-docs/test-docs
        install -m 755 "$2" /run/test-docs/server
        exec timeout 120 dbus-run-session -- /run/test-docs/test-docs protocol /run/test-docs/t /run/test-docs/server
    ' sh "$out/test-docs" "$out/$server" >"$out/protocol-$server.log" 2>&1 || status=1
    if grep -qx 'ALL PASSED' "$out/protocol-$server.log" && ! grep -qE 'Sanitizer|runtime error' "$out/protocol-$server.log"; then
        grep '^PASS' "$out/protocol-$server.log" | sed "s/^PASS/PASS ($server)/"
    else
        grep '^FAIL' "$out/protocol-$server.log" | sed "s/^FAIL/FAIL ($server)/" || true
        echo "FAIL the control protocol ($server): $out/protocol-$server.log"
        status=1
    fi
done
exit "$status"
