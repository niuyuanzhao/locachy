#!/bin/bash
# Device-free profile persistence and controller event regressions.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-monitor-profiles.sh NEW_ABSOLUTE_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || exit 2
mkdir -p "$out/protocols"
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base")
"${cc[@]}" "$src/tools/test-monitor-profiles.c" -o "$out/profiles"
"$out/profiles" "$out/fixtures"
"${cc[@]}" -fsanitize=address,undefined "$src/tools/test-monitor-profiles.c" -o "$out/profiles-san"
ASAN_OPTIONS=detect_leaks=0 "$out/profiles-san" "$out/fixtures-san"
for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 ext-idle-notify-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/protocols/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/protocols/$p-protocol.c"
done
"${cc[@]}" -ffunction-sections -fdata-sections -I"$out/protocols" "$src/tools/test-monitor-controller.c" \
    "$out/protocols/"*-protocol.c -Wl,--gc-sections -lwayland-client -lm -o "$out/controller"
"$out/controller"
printf '%s\n' 'PASS monitor profiles and controller event selection'
