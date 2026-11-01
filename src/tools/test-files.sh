#!/bin/sh
# test-files.sh NEW_OUTPUT: build test-files and the cloud folder worker, and
# run them as an unprivileged user (uid 1000) in a disposable user and mount
# namespace with a private /run: FILES_REQUEST is real there. The scratch home
# is two levels below /, as the history test going up to / expects.
set -eu
[ "$#" = 1 ] || { echo "usage: test-files.sh NEW_OUTPUT" >&2; exit 2; }
out=$(realpath -m "$1")
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
mkdir -p "$out"
src=$(cd "$(dirname "$0")/.." && pwd)
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/progs/files-cloud-list.c" \
    $(pkg-config --cflags --libs gio-2.0) -o "$out/files-cloud-list"
status=0
for mode in plain sanitize; do
    flags="-O2 -g"
    [ "$mode" = plain ] || flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"
    gcc $flags -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" \
        -DCLOUD_LIST='"/run/bin/files-cloud-list"' "$src/tools/test-files.c" "$src/base/ui.c" -o "$out/test-files-$mode"
    # The test user cannot enter the builder's home: the binaries are copied in.
    unshare --map-auto --map-root-user --mount sh -c '
        mount -t tmpfs -o mode=1777 tmpfs /run || exit 9
        mkdir -m 1777 /run/locachy /run/bin /run/scratch
        install -m 755 "$1" /run/scratch/test-files
        install -m 755 "$2" /run/bin/files-cloud-list
        exec setpriv --reuid=1000 --regid=1000 --clear-groups -- timeout 150 /run/scratch/test-files /run/fhome
    ' sh "$out/test-files-$mode" "$out/files-cloud-list" >"$out/$mode.log" 2>&1 || status=1
    if grep -q '^ALL PASSED$' "$out/$mode.log" && ! grep -qE 'Sanitizer|runtime error' "$out/$mode.log"; then
        echo "PASS test-files ($mode): $(grep -c '^== ' "$out/$mode.log") scenarios"
    else
        echo "FAIL test-files ($mode): see $out/$mode.log"
        status=1
    fi
done
exit "$status"
