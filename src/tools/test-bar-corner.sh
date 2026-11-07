#!/bin/bash
# Compile the real bar corner state machine with dead unused paths removed.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-bar-corner.sh NEW_ABSOLUTE_OUTPUT_DIRECTORY}
[[ $out = /* && ! -e $out ]] || { printf '%s\n' 'Output must be a new absolute directory' >&2; exit 2; }
mkdir -p "$out/protocols"
for p in wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 \
         wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter \
         relative-pointer-unstable-v1 xdg-shell; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/protocols/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/protocols/$p-protocol.c"
done
gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections \
    $(pkg-config --cflags gio-2.0) -I"$src/base/include" -I"$src/base" -I"$out/protocols" \
    "$src/tools/test-bar-corner.c" "$out/protocols/"*-protocol.c \
    $(pkg-config --libs gio-2.0 xkbcommon wayland-client wayland-cursor) \
    -Wl,--gc-sections -lm -o "$out/test"
"$out/test"
