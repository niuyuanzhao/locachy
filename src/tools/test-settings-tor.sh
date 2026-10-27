#!/bin/sh
# test-settings-tor.sh [DIR]: settings' Tor rows (test-settings-tor.c), built
# plain and with AddressSanitizer and UBSan, run on scripts of its own in
# place of tor-enforce and tor-client. Touches nothing outside DIR.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-settings-tor}
rm -rf "$out"
mkdir -p "$out"
out=$(realpath "$out")
cc=${CC:-cc}
for kind in plain sanitized; do
    extra=
    [ "$kind" = plain ] || extra='-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
    # shellcheck disable=SC2086
    "$cc" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections $extra \
        -I"$src/base/include" -I"$src/base" "-DTOR_TEST_DIR=\"$out/$kind\"" \
        "$src/tools/test-settings-tor.c" -Wl,--gc-sections -o "$out/test-settings-tor-$kind"
    ASAN_OPTIONS=detect_leaks=0 "$out/test-settings-tor-$kind"
done
echo 'PASS settings: Tor rows, plain and AddressSanitizer/UBSan'
