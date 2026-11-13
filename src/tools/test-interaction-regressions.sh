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
# The key action table (base/key-action.c) and the help panel (base/help.c,
# ui_help) are part of every program now; they need only base.h, and
# --gc-sections drops them where a test does not use them.
link=("$src/base/key-action.c" "$src/base/help.c" -Wl,--gc-sections)

"${cc[@]}" "$src/tools/test-home.c" "$src/base/ui.c" "$src/base/textsel.c" "${link[@]}" -o "$out/home"
"$out/home" "$out/home-fixtures"
"${cc[@]}" "$src/tools/test-settings-monitors.c" "${link[@]}" -o "$out/settings"
"$out/settings" "$out/settings-fixtures"
"${cc[@]}" "$src/tools/test-settings-close.c" "${link[@]}" -o "$out/settings-close"
"$out/settings-close"
"${cc[@]}" "$src/tools/test-settings-optional.c" "${link[@]}" -o "$out/settings-optional"
"$out/settings-optional"
"${cc[@]}" "$src/tools/test-term-close.c" "${link[@]}" -o "$out/term-close"
timeout 30 "$out/term-close" "$out/term-close-fixtures"
"${cc[@]}" "$src/tools/test-monitor-layout.c" "${link[@]}" -o "$out/monitor-layout"
"$out/monitor-layout"
"${cc[@]}" "$src/tools/test-packages.c" "$src/base/ui.c" "$src/base/rc.c" "$src/base/textsel.c" "${link[@]}" \
    -o "$out/packages"
timeout 180 "$out/packages" "$out/packages-fixtures"
"${cc[@]}" "$src/tools/test-firmware.c" "$src/base/ui.c" "${link[@]}" -o "$out/firmware"
timeout 60 "$out/firmware" "$out/firmware-fixtures"
"${cc[@]}" $(pkg-config --cflags json-c) "$src/tools/test-installer.c" "$src/base/ui.c" "$src/base/textsel.c" \
    "${link[@]}" $(pkg-config --libs json-c) -o "$out/installer"
timeout 60 "$out/installer" "$out/installer-fixtures"
"${cc[@]}" $(pkg-config --cflags json-c) "$src/tools/test-rescue.c" "$src/base/ui.c" "$src/base/textsel.c" \
    "${link[@]}" $(pkg-config --libs json-c) -o "$out/rescue"
timeout 90 "$out/rescue" "$out/rescue-fixtures"
"${cc[@]}" "$src/progs/keytable.c" $(pkg-config --cflags --libs xkbcommon) -o "$out/keytable"
"${cc[@]}" "$src/tools/test-keytable.c" "${link[@]}" -o "$out/keytable-test"
LC_ALL=C.UTF-8 "$out/keytable-test" "$out/keytable" "$out/keytable-fixtures"
"$src/tools/test-app-registry.sh" "$out/app-registry"
# The input method's keys per mode (English, Chinese, direct, a window) with a double of Rime's session.
"${cc[@]}" -DCONFIG_IME -I"$src/tools/rime-double" "$src/tools/test-ime-keys.c" "${link[@]}" -o "$out/ime-keys"
"$out/ime-keys"
for test in key-actions keys-name keyboard-devices term-clipboard screen-mirror modules-event monitor-modes lock-layout panel-hits; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -o "$out/$test"
    "$out/$test"
done
# Quit and the clipboard come from key_action in every program, never from raw key values.
# seat's fallback labwc configuration: Super+Esc stays a home key without the generated one.
"${cc[@]}" "$src/tools/test-labwc-fallback.c" "${link[@]}" -o "$out/labwc-fallback"
mkdir -p "$out/labwc-fallback-fixtures"
"$out/labwc-fallback" "$out/labwc-fallback-fixtures"
python3 -I "$src/tools/test-key-literals.py" "$src"
cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" "$src/tools/test-tor-browser.c" -o "$out/tor-browser"
"$out/tor-browser"
# keyboard-locks: one Caps/Num Lock across process 1, a direct program and the shared display.
for test in pointer input-hotplug keyboard-locks; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -lm -o "$out/$test"
    "$out/$test"
done
# init-shutdown: a power off waiting for a system job; link/poweroff and reboot in user and PID namespaces.
# init-services: process 1's rc.conf reload (SIGHUP), its growth, restarts and a failed reload.
for test in rtc-alarm suspend-hibernate init-lid init-shutdown init-services status-battery battery-handoff; do
    "${cc[@]}" "$src/tools/test-$test.c" "${link[@]}" -o "$out/$test"
    "$out/$test" "$out/$test-fixtures"
done
"${cc[@]}" -fsanitize=address,undefined "$src/tools/test-battery-handoff.c" -o "$out/battery-handoff-san"
"$out/battery-handoff-san" "$out/battery-handoff-san-fixtures"
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
    "$src/tools/test-bar-keyboard.c" "$src/base/keys.c" "$src/base/control.c" "$out/protocols"/*-protocol.c \
    $(pkg-config --libs gio-2.0 xkbcommon wayland-client wayland-cursor) -lm \
    "${link[@]}" -o "$out/bar-keyboard"
"$out/bar-keyboard"
bash "$src/tools/test-bar-corner.sh" "$out/bar-corner"
# Host notifications on a private session bus, through the real bar server.
"${cc[@]}" $(pkg-config --cflags gio-2.0) -I"$out/protocols" "$src/tools/test-host-notify.c" \
    "$out/protocols"/*-protocol.c $(pkg-config --libs gio-2.0 wayland-client wayland-cursor xkbcommon) -lm \
    "${link[@]}" -o "$out/host-notify"
timeout 30 dbus-run-session -- "$out/host-notify"
"${cc[@]}" "$src/tools/test-drives-state.c" "${link[@]}" -o "$out/drives-state"
"$out/drives-state" "$out/drives-fixtures"
"${cc[@]}" -fsanitize=address,undefined "$src/tools/test-drives-eject.c" -o "$out/drives-eject"
"$out/drives-eject" "$out/drives-eject-fixtures"
for test in gestures libinput audio-packages audio-client audio-isolation face-camera container-migration kernel-choice \
            boot-kernel root-boot-unit install apps-catalog host-app-build declare-packages efi init-unlock init-storage libpaper rclone \
            local-packages local-build repos firmware pkg-sync; do
    python3 -B "$src/tools/test-$test.py" "$out"
done
for test in xwayland tpm jobs-routing rescue-dispatch rescue-storage rescue-link; do # no scratch directory
    python3 -B "$src/tools/test-$test.py"
done
bash "$src/tools/test-timezone-client.sh" "$out/timezone"
bash "$src/tools/test-monitor-profiles.sh" "$out/monitor-profiles"
sh "$src/tools/test-files.sh" "$out/files"
sh "$src/tools/test-fonts.sh" "$out/fonts" # fontconfig's fonts in fixture configurations
sh "$src/tools/test-app-init.sh" "$out/app-init"
sh "$src/tools/test-seat-release.sh" "$out/seat-release"
bash "$src/tools/test-window-routing.sh" "$out/window-routing"
sh "$src/tools/test-container-documents.sh" "$out/container-documents"
sh "$src/tools/test-docs.sh" "$out/docs" # the document service: user namespaces, a stand-in portal
bash "$src/tools/test-jobs.sh" "$out/jobs" # the system jobs' queue: user namespaces, fixture cgroups
bash "$src/tools/test-docs-portal.sh" "$out/docs-portal" # the packaged document portal (FUSE) in a user namespace
bash "$src/tools/test-camera-nodes.sh" "$out/camera-nodes"
# Touchpad Clicks: the real libinput through pointer.c, devices described in user and mount namespaces.
bash "$src/tools/test-touchpad-clicks.sh" "$out/touchpad-clicks"
bash "$src/tools/test-app-ready.sh" "$out/app-ready"
bash "$src/tools/test-network-handoff.sh" "$out/network-handoff" # unprivileged user and network namespaces
bash "$src/tools/test-settings-tor.sh" "$out/settings-tor"
# The security policy job, shared configuration transactions and generated app files (no scratch directory);
# the external pacman guard and the update journal; the archive worker against its own build.
for test in security config-transactions app-generation; do
    python3 -B "$src/tools/test-$test.py"
done
for test in pacman-guard update-transaction; do
    python3 -B "$src/tools/test-$test.py" "$out"
done
"${cc[@]}" "$src/progs/archive-worker.c" $(pkg-config --cflags --libs libarchive libcrypto) -o "$out/archive-worker"
python3 -B "$src/tools/test-archive-worker.py" "$out/archive-worker" "$out"
# The archive manager by its keys, over that worker (as the user, not root): the screen, the keys and the file chooser are doubles.
"${cc[@]}" -DARCHIVE_WORKER_TEST_PATH="\"$out/archive-worker\"" "$src/tools/test-archive.c" "$src/base/ui.c" \
    "${link[@]}" -o "$out/archive"
"$out/archive" "$out/archive-fixtures"
# The extended display's receiver and pairing client (FFmpeg, IPC, a stub broker) and the direct screen's power.
sh "$src/tools/test-extdisplay.sh"
# The broker: the pairing code, the cable TLS profile, and the lease in user and network namespaces.
sh "$src/tools/test-extdisplay-broker.sh" "$out/extdisplay-broker"
sh "$src/tools/test-screen-power.sh" "$out/screen-power"
printf '%s\n' 'PASS device-free interaction regressions; real KMS/libinput/IME hardware acceptance remains separate.'
