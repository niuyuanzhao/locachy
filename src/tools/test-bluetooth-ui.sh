#!/bin/bash
# Exact packaged bluetui, standard python-dbusmock BlueZ template, real term
# parser. Simulation only: no Bluetooth hardware, display or input devices.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "$src/.bt-ui-test-XXXXXXXX")
printf 'Bluetooth UI test artifacts: %s\n' "$out"
cc=${CC:-gcc}
"$cc" -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$src/base/include" "$src/tools/test-bluetooth-ui-grid.c" \
    -Wl,--gc-sections -o "$out/grid"
export src out
# A private devpts provides the PTY even in a disposable chroot with no /dev/pts.
# The private network namespace cannot use host HCI controllers.
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc --net \
    bash -euc '
        mkdir -p /dev/pts
        mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts /dev/pts
        ln -sfn pts/ptmx /dev/ptmx
        timeout 150 python3 "$src/tools/test-bluetooth-ui.py" --grid "$out/grid" --out "$out"
    '
