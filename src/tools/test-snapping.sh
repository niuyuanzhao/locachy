#!/bin/bash
# Actual window drags on two and three private headless outputs: exterior
# and shared boundaries, corners, maximize, fractional scale, negative positions.
# No uinput, physical input or display. Run as root in a disposable test root.
# The output directory must be new; logs and screenshots are retained there.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-snapping.sh NEW_ABSOLUTE_OUTPUT_DIRECTORY}
[[ $out = /* && ! -e $out ]] || exit 2
mkdir -p "$out/gen" "$out/etc/labwc"
for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 \
         wlr-output-management-unstable-v1 wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 \
         xdg-shell wlr-screencopy-unstable-v1 wlr-virtual-pointer-unstable-v1 \
         fractional-scale-v1 viewporter relative-pointer-unstable-v1 ext-idle-notify-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/gen/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/gen/$p-protocol.c"
done
dec=$src/base/protocols/xdg-decoration-unstable-v1.xml
wayland-scanner client-header "$dec" "$out/gen/xdg-decoration-unstable-v1-client-protocol.h"
wayland-scanner private-code "$dec" "$out/gen/xdg-decoration-unstable-v1-protocol.c"
cc=(gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -I"$out/gen")
mapfile -t base < <(find "$src/base" -maxdepth 1 -name '*.c' ! -name ime.c | sort)
read -r -a flags <<<"$(pkg-config --cflags freetype2 libnm)"
read -r -a libs <<<"$(pkg-config --libs freetype2 libnm)"
"${cc[@]}" "${flags[@]}" -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER \
    "$src/progs/labwc-control.c" "${base[@]}" "$out/gen/security-context-v1-protocol.c" \
    "$out/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.c" \
    "$out/gen/wlr-output-management-unstable-v1-protocol.c" "$out/gen/ext-idle-notify-v1-protocol.c" \
    "${libs[@]}" -lwayland-client -lm -o "$out/control"
"${cc[@]}" "${flags[@]}" -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER \
    "$src/tools/test-output-state.c" "${base[@]}" "$out/gen/security-context-v1-protocol.c" \
    "$out/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.c" \
    "$out/gen/wlr-output-management-unstable-v1-protocol.c" "$out/gen/ext-idle-notify-v1-protocol.c" \
    "${libs[@]}" -lwayland-client -lm -o "$out/outputs"
# Rime is optional on the target host; snapping does not need an input method.
ime_flags=() ime_libs=()
if [ -f /usr/include/rime_api.h ] && [ -f /usr/lib/librime.so ]; then
    ime_flags=(-DCONFIG_IME)
    rime_lua=$({ ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null || true; } | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    ime_libs=(-lrime ${rime_lua:+-Wl,--push-state,--no-as-needed -l:$rime_lua -Wl,--pop-state})
fi
"${cc[@]}" "${flags[@]}" -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER "${ime_flags[@]}" \
    "$src/progs/bar.c" "${base[@]}" "$src/base/ime.c" \
    "$out/gen/wlr-layer-shell-unstable-v1-protocol.c" "$out/gen/virtual-keyboard-unstable-v1-protocol.c" \
    "$out/gen/xdg-shell-protocol.c" "$out/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.c" \
    "$out/gen/fractional-scale-v1-protocol.c" "$out/gen/viewporter-protocol.c" \
    "$out/gen/relative-pointer-unstable-v1-protocol.c" "${libs[@]}" -lwayland-client -lm "${ime_libs[@]}" \
    -lxkbcommon -lwayland-cursor -o "$out/bar"
# These two clients define _GNU_SOURCE themselves.
"${cc[@]}" -U_GNU_SOURCE "$src/tools/test-snap-window.c" "$out/gen/xdg-shell-protocol.c" \
    "$out/gen/xdg-decoration-unstable-v1-protocol.c" -lwayland-client -o "$out/window"
"${cc[@]}" -U_GNU_SOURCE "$src/tools/test-snap-drag.c" "$out/gen/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -lwayland-client -lm -o "$out/drag"
"${cc[@]}" "$src/tools/test-screenshot.c" "$out/gen/wlr-screencopy-unstable-v1-protocol.c" \
    -lwayland-client -o "$out/shot"
cp "$src/rc.conf" "$out/etc/rc.conf"
printf '\nlock_after="0"\n' >>"$out/etc/rc.conf"
mkdir -p "$out/etc/containers"
python3 - "$src/link/labwc-rc.xml" "$out/etc/labwc/rc.xml" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text().replace('TERMINAL-KEY', 'C-A-t').replace('HOME-KEY', 'W-Escape')
text = ''.join(line for line in text.replace('LOCK-KEY', 'W-l').splitlines(True) if 'IME-KEY' not in line)
# Only deterministic starting placement is added. Do not set snap ranges:
# testing the shipped config and the installed compositor's real defaults.
text = text.replace('</openbox_config>', '''<windowRules>
<windowRule identifier="locachy-snap-test"><action name="MoveTo" x="300" y="200"/></windowRule>
</windowRules></openbox_config>''')
Path(sys.argv[2]).write_text(text)
PY
test_script=test-snapping.py
[ "${2:-}" != --inner-edge ] || test_script=test-inner-edge.py
export src out test_script LOCACHY_TEST_LABWC="${LOCACHY_TEST_LABWC:-labwc}"
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mkdir -p /dev/input /run/user /run/locachy /etc/locachy
    mount -t tmpfs -o mode=0755 tmpfs /dev/input
    mount -t tmpfs -o mode=0755 tmpfs /run/user
    mount -t tmpfs -o mode=0755 tmpfs /run/locachy
    mkdir -m 0700 /run/user/0
    mount --bind "$out/etc" /etc/locachy
    export XDG_RUNTIME_DIR=/run/user/0
    export LOCACHY_TEST_LABWC
    python3 "$src/tools/$test_script" "$out"
'
