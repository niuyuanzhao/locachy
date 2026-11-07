#!/bin/bash
# Touchpad Clicks with the real libinput (pkg-config's) through base/pointer.c,
# without devices or root: in a user and mount namespace of its own, tmpfs
# over /sys/devices/virtual/mem, /sys/dev/char and /run/udev holds what udev
# and sysfs would say of a clickpad, a TrackPoint and a two-button touchpad;
# /dev/null, /dev/zero and /dev/full are bound at their nodes, a /dev of the
# test's own. libinput reads settings' section from its local overrides
# file, a directory of the test's bound at /etc/libinput (or, without that
# directory, the packaged quirks and the section through LIBINPUT_QUIRKS_DIR).
# Also checks that labwc's rc.xml has the lines settings writes.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-touchpad-clicks.sh NEW_OUTPUT_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || { echo 'Output must be a new absolute directory' >&2; exit 2; }
grep -q '^      <tap>yes</tap>$' "$src/link/labwc-rc.xml" &&
    grep -q '^      <clickMethod></clickMethod>$' "$src/link/labwc-rc.xml" ||
    { echo 'FAIL labwc-rc.xml lacks the tap and clickMethod lines settings writes'; exit 1; }
[ "$(grep -c '<tap>\|<clickMethod>' "$src/link/labwc-rc.xml")" = 2 ] ||
    { echo 'FAIL labwc-rc.xml names tap or clickMethod elsewhere (settings rewrites every such line)'; exit 1; }
echo 'PASS labwc-rc.xml: one tap line (yes, as labwc defaults) and one empty clickMethod line for settings'
# labwc reads the acceleration as pointerSpeed (src/config/rcxml.c) and
# ignores unknown elements silently.
[ "$(grep -c '^      <pointerSpeed>0\.4</pointerSpeed>$' "$src/link/labwc-rc.xml")" = 1 ] &&
    ! grep -q 'pointerAcceleration' "$src/link/labwc-rc.xml" "$src/progs/settings.c" ||
    { echo 'FAIL labwc-rc.xml or settings does not use labwc pointerSpeed for the acceleration'; exit 1; }
echo 'PASS labwc-rc.xml and settings write the acceleration as pointerSpeed, the element labwc reads'
mkdir -p "$out/dev/input" "$out/etc-libinput" "$out/quirks"
read -r -a libinput_flags <<<"$(pkg-config --cflags --libs libinput)"
gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" \
    "$src/tools/test-touchpad-clicks.c" "${libinput_flags[@]}" -lm -o "$out/touchpad-clicks"
for n in 100 101 102; do touch "$out/dev/input/event$n"; done
for n in null zero full; do touch "$out/dev/$n"; done
ln -s /proc/self/fd "$out/dev/fd"
if [ -d /etc/libinput ]; then
    quirks=/etc/libinput/local-overrides.quirks
else
    cp /usr/share/libinput/*.quirks "$out/quirks/"
    quirks=$out/quirks/zz-local-overrides.quirks
fi
unshare --map-root-user --mount bash -euc '
    out=$1 quirks=$2
    mount --bind /dev/null "$out/dev/input/event100"
    mount --bind /dev/zero "$out/dev/input/event101"
    mount --bind /dev/full "$out/dev/input/event102"
    for n in null zero full; do mount --bind "/dev/$n" "$out/dev/$n"; done
    mount --rbind "$out/dev" /dev
    for d in /sys/devices/virtual/mem /sys/dev/char /run/udev; do mount -t tmpfs fake "$d"; done
    mkdir /run/udev/data
    if [ "$quirks" = /etc/libinput/local-overrides.quirks ]; then
        mount --bind "$out/etc-libinput" /etc/libinput
    else
        export LIBINPUT_QUIRKS_DIR=$out/quirks
    fi
    exec timeout 120 "$out/touchpad-clicks" "$quirks"
' bash "$out" "$quirks" 2>&1 | tee "$out/touchpad-clicks.log"
grep -q '^PASS back to tap and press' "$out/touchpad-clicks.log"
