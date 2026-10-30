#!/bin/bash
# Device-free Linux regression suite; real KMS and libinput run in separate VMs.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-interaction-regressions.sh NEW_OUTPUT_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || {
    printf '%s\n' 'Output must be a new absolute directory' >&2
    exit 2
}
mkdir -p "$out"
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections
    -I"$src/base/include" -I"$src/base")
link=(-Wl,--gc-sections)

"${cc[@]}" "$src/tools/test-home.c" "$src/base/ui.c" "${link[@]}" -o "$out/home"
"$out/home"
"${cc[@]}" "$src/tools/test-settings-monitors.c" "${link[@]}" -o "$out/settings"
"$out/settings" "$out/settings-fixtures"
"${cc[@]}" "$src/tools/test-monitor-layout.c" "${link[@]}" -o "$out/monitor-layout"
"$out/monitor-layout"
"${cc[@]}" "$src/tools/test-packages.c" "$src/base/ui.c" "$src/base/rc.c" "${link[@]}" -o "$out/packages"
timeout 180 "$out/packages" "$out/packages-fixtures"
"${cc[@]}" $(pkg-config --cflags json-c) "$src/tools/test-installer.c" "$src/base/ui.c" "${link[@]}" \
    $(pkg-config --libs json-c) -o "$out/installer"
timeout 60 "$out/installer" "$out/installer-fixtures"
"${cc[@]}" "$src/progs/keytable.c" $(pkg-config --cflags --libs xkbcommon) -o "$out/keytable"
"${cc[@]}" "$src/tools/test-keytable.c" "${link[@]}" -o "$out/keytable-test"
LC_ALL=C.UTF-8 "$out/keytable-test" "$out/keytable" "$out/keytable-fixtures"
for test in keys-name keyboard-devices term-clipboard screen-mirror; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -o "$out/$test"
    "$out/$test"
done
for test in pointer input-hotplug; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -lm -o "$out/$test"
    "$out/$test"
done
for test in rtc-alarm suspend-hibernate status-battery; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -o "$out/$test"
    "$out/$test" "$out/$test-fixtures"
done
"${cc[@]}" "$src/tools/test-init-screen.c" "${link[@]}" -o "$out/init-screen"
# frame.cgroup models the short production cgroup path, not a long result path.
(cd "$out" && ./init-screen init-fixtures)
mkdir "$out/protocols"
for protocol in wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 \
    wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter \
    relative-pointer-unstable-v1 xdg-shell; do
    wayland-scanner client-header "$src/base/protocols/$protocol.xml" "$out/protocols/$protocol-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$protocol.xml" "$out/protocols/$protocol-protocol.c"
done
"${cc[@]}" $(pkg-config --cflags gio-2.0) -I"$out/protocols" \
    "$src/tools/test-bar-keyboard.c" "$src/base/keys.c" "$out/protocols"/*-protocol.c \
    $(pkg-config --libs gio-2.0 xkbcommon wayland-client wayland-cursor) -lm \
    "${link[@]}" -o "$out/bar-keyboard"
"$out/bar-keyboard"
for test in gestures libinput audio-packages audio-client face-camera container-migration kernel-choice \
            boot-kernel root-boot-unit install declare-packages efi init-unlock libpaper; do
    python3 -B "$src/tools/test-$test.py" "$out"
done
for test in xwayland tpm; do # no scratch directory
    python3 -B "$src/tools/test-$test.py"
done
bash "$src/tools/test-timezone-client.sh" "$out/timezone"
bash "$src/tools/test-settings-tor.sh" "$out/settings-tor"
printf '%s\n' 'PASS device-free interaction regressions; real KMS/libinput/IME hardware acceptance remains separate.'
