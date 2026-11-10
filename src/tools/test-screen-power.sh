#!/bin/sh
# base_screen_power (base/screen.c) against a recording fake of its DRM calls,
# injected at compile time (SCREEN_POWER_IOCTL); no device, no display. Built
# as the programs build screen.c (glibc), sanitized, and with musl as process
# 1 builds it. Judged by the PASS/FAIL lines.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-screen-power.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
flags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/base/include -I$src/base
    -ffunction-sections -fdata-sections -Wl,--gc-sections"
# shellcheck disable=SC2086
gcc $flags "$src/tools/test-screen-power.c" -o "$out/test-glibc"
# shellcheck disable=SC2086
gcc $flags -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
    "$src/tools/test-screen-power.c" -o "$out/test-sanitize"
builds="glibc sanitize"
if command -v musl-gcc >/dev/null 2>&1; then
    mkdir -p "$out/uapi"
    for d in linux asm asm-generic drm; do ln -sfn "/usr/include/$d" "$out/uapi/$d"; done
    # shellcheck disable=SC2086
    musl-gcc -static $flags -isystem "$out/uapi" "$src/tools/test-screen-power.c" -o "$out/test-musl"
    builds="$builds musl"
fi
status=0
for build in $builds; do
    "$out/test-$build" >"$out/$build.log" 2>&1 || true
    sed "s/^/$build: /" "$out/$build.log"
    if grep -q '^ALL PASSED$' "$out/$build.log" && ! grep -q '^FAIL' "$out/$build.log"; then
        echo "PASS test-screen-power ($build)"
    else
        echo "FAIL test-screen-power ($build)"
        status=1
    fi
done
exit $status
