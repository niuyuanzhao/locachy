#!/usr/bin/env bash
# Isolated Linux-only regression tests: pipes, socketpairs and libinput
# lifecycle doubles. No physical input, uinput, real display, or PID 1.
set -euo pipefail
source=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d /tmp/input-hotplug.XXXXXX)
trap 'rm -rf "$build"' EXIT
for test in input-hotplug input-libinput-hotplug; do
    cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -I"$source/base/include" -I"$source/init" \
        "$source/tools/test-$test.c" -Wl,--gc-sections \
        $(pkg-config --cflags --libs libinput) -lm -o "$build/$test"
    timeout 20 "$build/$test"
done
