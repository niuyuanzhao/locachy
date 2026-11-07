#!/bin/bash
# The drop gate of private-file drags on a private headless compositor: a
# host source offering application/vnd.portal.filetransfer (also one in a
# native window's cgroup, app_window-files-1, as files runs), a host target,
# a native window target and a container target (locachy-0 and an app_
# cgroup), and a stand-in document
# service on /run/locachy/docs/control answering ok, refuse, late or never;
# Escape during a drag cancels it. Drags come from a virtual pointer, Escape
# from a virtual keyboard: no uinput, physical input or display.
# Run as root in a disposable test root with cgroup2 at /sys/fs/cgroup. OUT
# must be a new absolute directory; logs stay there.
#   test-drop-gate.sh OUT [LABWC]
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-drop-gate.sh NEW_ABSOLUTE_OUTPUT_DIRECTORY [LABWC]}
labwc=${2:-/usr/bin/labwc}
[[ $out = /* && ! -e $out ]] || exit 2
mkdir -p "$out/gen" "$out/etc/labwc" "$out/etc/containers"
for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 \
         wlr-output-management-unstable-v1 ext-idle-notify-v1 xdg-shell wlr-virtual-pointer-unstable-v1 \
         virtual-keyboard-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/gen/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/gen/$p-protocol.c"
done
cc=(gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -I"$out/gen")
mapfile -t base < <(find "$src/base" -maxdepth 1 -name '*.c' ! -name ime.c | sort)
read -r -a flags <<<"$(pkg-config --cflags freetype2 fontconfig libnm)"
read -r -a libs <<<"$(pkg-config --libs freetype2 fontconfig libnm)"
"${cc[@]}" "${flags[@]}" -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER \
    "$src/progs/labwc-control.c" "${base[@]}" "$out/gen/security-context-v1-protocol.c" \
    "$out/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.c" \
    "$out/gen/wlr-output-management-unstable-v1-protocol.c" "$out/gen/ext-idle-notify-v1-protocol.c" \
    "${libs[@]}" -lwayland-client -lm -o "$out/control"
# These two clients define _GNU_SOURCE themselves.
"${cc[@]}" -U_GNU_SOURCE "$src/tools/test-drop-client.c" "$out/gen/xdg-shell-protocol.c" \
    "$out/gen/virtual-keyboard-unstable-v1-protocol.c" -lwayland-client -lxkbcommon -o "$out/client"
"${cc[@]}" -U_GNU_SOURCE "$src/tools/test-snap-drag.c" "$out/gen/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -lwayland-client -lm -o "$out/drag"
cp "$src/rc.conf" "$out/etc/rc.conf"
printf '\nlock_after="0"\n' >>"$out/etc/rc.conf"
: >"$out/etc/monitors.conf"
python3 - "$src/link/labwc-rc.xml" "$out/etc/labwc/rc.xml" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text().replace('TERMINAL-KEY', 'C-A-t').replace('HOME-KEY', 'W-Escape')
text = ''.join(line for line in text.replace('LOCK-KEY', 'W-l').splitlines(True) if 'IME-KEY' not in line)
# Only fixed, apart window positions are added to the shipped configuration.
text = text.replace('</openbox_config>', '''<windowRules>
<windowRule identifier="locachy-drop-source"><action name="MoveTo" x="40" y="60"/></windowRule>
<windowRule identifier="locachy-drop-target"><action name="MoveTo" x="700" y="60"/></windowRule>
</windowRules></openbox_config>''')
Path(sys.argv[2]).write_text(text)
PY
group=$(mktemp -d /sys/fs/cgroup/locachy-drop-test.XXXXXX)
# A container (app_demo) and two native windows (app_window-*: root host
# programs as process 1 runs files and view).
groups="app_demo app_window-files-1 app_window-view-1"
cleanup() {
    for g in $groups; do
        [ -d "$group/$g" ] || continue
        printf '1\n' >"$group/$g/cgroup.kill"
        for _ in $(seq 1 50); do rmdir "$group/$g" 2>/dev/null && break; sleep 0.1; done
    done
    rmdir "$group" 2>/dev/null || true
}
trap cleanup EXIT
for g in $groups; do mkdir "$group/$g"; done
export src out group labwc
unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -euc '
    mkdir -p /run/user /run/locachy /etc/locachy
    mount -t tmpfs -o mode=0755 tmpfs /run/user
    mount -t tmpfs -o mode=0755 tmpfs /run/locachy
    mkdir -m 0700 /run/user/0
    mount --bind "$out/etc" /etc/locachy
    export XDG_RUNTIME_DIR=/run/user/0
    python3 -B "$src/tools/test-drop-gate.py" "$out" "$group" "$labwc"
'
