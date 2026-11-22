#!/bin/sh
# The extended display's touch and tablet input (tools/EXTDISPLAY.md): the
# protocol's capability negotiation and JSON formatters, the droppable rules,
# the pointer layer's raw direct queue, and that the libinput build of
# base/pointer.c still compiles. No device, display or process 1 is involved;
# the libinput mapping itself is exercised by test-pointer-libinput.sh in a VM.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-extdisplay-input.XXXXXX")
trap 'rm -rf "$out"' EXIT
if ! pkg-config --exists json-c; then
    echo "SKIP: json-c is not installed" >&2
    exit 77
fi
cflags="-std=gnu11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/progs -I$src/base/include -I$src/base"
cc $cflags $(pkg-config --cflags json-c) "$src/tools/test-extdisplay-input.c" \
    "$src/progs/extdisplay-protocol.c" $(pkg-config --libs json-c) -lm -o "$out/test-extdisplay-input"
"$out/test-extdisplay-input"
if pkg-config --exists libinput; then
    cc $cflags -DCONFIG_LIBINPUT $(pkg-config --cflags libinput) -c "$src/base/pointer.c" -o "$out/pointer-libinput.o"
    echo "PASS: base/pointer.c builds with libinput (the device mapping is verified in a VM)"
else
    echo "SKIP: libinput is not installed; base/pointer.c was not built with it" >&2
fi
