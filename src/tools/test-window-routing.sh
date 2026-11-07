#!/bin/bash
# Device-free window request routing: locachy-seat --window (client and the
# seat's forwarding to process 1) and labwc-control's part. Process 1's
# registry of the windows is test-app-registry.sh's.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-window-routing.sh NEW_ABSOLUTE_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || { printf '%s\n' 'Output must be a new absolute directory' >&2; exit 2; }
mkdir -p "$out/protocols"
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base")
"${cc[@]}" -ffunction-sections -fdata-sections "$src/tools/test-seat-window.c" -Wl,--gc-sections \
    -o "$out/seat-window"
timeout 20 "$out/seat-window"
for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 \
         ext-idle-notify-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/protocols/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/protocols/$p-protocol.c"
done
"${cc[@]}" -ffunction-sections -fdata-sections -I"$out/protocols" "$src/tools/test-control-window.c" \
    "$out/protocols/"*-protocol.c -Wl,--gc-sections -lwayland-client -lm -o "$out/control-window"
timeout 20 "$out/control-window"
printf '%s\n' 'PASS window request routing (seat client, seat, labwc-control)'
