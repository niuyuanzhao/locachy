#!/usr/bin/env bash
# Boot a disposable, diskless VM with uinput, udevd and libinput copied from
# this machine, and run test-pointer-libinput there; never create uinput
# devices on the host. Its round touchpads need LOCachy's libinput
# (pkgbuilds/libinput); on a machine with another, LIBINPUT_SOURCE names a
# source tree of it built in its build/ (meson), whose library and quirks the
# VM gets instead. The VM's DMI says Panasonic CF-FV1, for libinput's quirk.
set -euo pipefail
if (( $# != 3 )); then
    printf 'Usage: %s KERNEL UINPUT_MODULE OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi
KERNEL=$(realpath "$1")
MODULE=$(realpath "$2")
OUT=$(realpath -m "$3")
SOURCE=$(cd "$(dirname "$0")/.." && pwd)
test -r "$KERNEL"
test -r "$MODULE"
if [[ ${PREPARE_ONLY:-0} != 1 ]]; then
    test -r /dev/kvm && test -w /dev/kvm
fi
if [[ -e "$OUT" ]]; then
    printf 'Output directory already exists: %s\n' "$OUT" >&2
    exit 2
fi
ROOT=$OUT/root
mkdir -p "$ROOT"/{bin,etc,usr/bin,usr/lib/systemd,usr/lib/udev,usr/share}
if [[ -n ${LIBINPUT_SOURCE:-} ]]; then
    LIBINPUT_SOURCE=$(realpath "$LIBINPUT_SOURCE")
    test -r "$LIBINPUT_SOURCE/build/libinput.so.10"
    libinput_flags=(-I "$LIBINPUT_SOURCE/src" -L "$LIBINPUT_SOURCE/build" -Wl,-rpath,"$LIBINPUT_SOURCE/build" -linput)
else
    read -r -a libinput_flags <<<"$(pkg-config --cflags --libs libinput)"
fi
gcc -O2 -Wall -Wextra -Werror -I "$SOURCE/base/include" -I "$SOURCE/base" \
    "$SOURCE/tools/test-pointer-libinput.c" "${libinput_flags[@]}" -lm \
    -o "$ROOT/test-pointer-libinput"

# The libraries a program needs, at their paths (the loader too).
libraries() {
    ldd "$1" | awk '/=> \//{print $3} /^[[:space:]]*\/[^ ]*ld-linux/{print $1}' | while read -r lib; do
        [[ -e "$ROOT$lib" ]] || install -D -m 0755 "$(realpath "$lib")" "$ROOT$lib"
    done
}
libraries "$ROOT/test-pointer-libinput"
install -m 0755 /usr/bin/udevadm "$ROOT/usr/bin/udevadm"
libraries /usr/bin/udevadm
ln -s ../../bin/udevadm "$ROOT/usr/lib/systemd/systemd-udevd"
cp -a /usr/lib/udev/rules.d "$ROOT/usr/lib/udev/"
install -m 0444 /usr/lib/udev/hwdb.bin "$ROOT/usr/lib/udev/hwdb.bin"
for helper in /usr/lib/udev/libinput-*; do
    install -m 0755 "$helper" "$ROOT$helper"
    libraries "$helper"
done
cp -a /usr/share/libinput "$ROOT/usr/share/"
if [[ -n ${LIBINPUT_SOURCE:-} ]]; then
    rm -f "$ROOT"/usr/share/libinput/*.quirks
    install -m 0644 "$LIBINPUT_SOURCE"/quirks/*.quirks "$ROOT/usr/share/libinput/"
fi
[[ ! -d /usr/share/libwacom ]] || cp -a /usr/share/libwacom "$ROOT/usr/share/"
install -m 0755 "${BUSYBOX:-/usr/bin/busybox}" "$ROOT/bin/busybox"
ln -s busybox "$ROOT/bin/sh"
printf 'root:x:0:0::/:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\ninput:x:97:\nvideo:x:91:\ntty:x:5:\n' > "$ROOT/etc/group"
case "$MODULE" in
    *.zst) zstd -d -q -c "$MODULE" > "$ROOT/uinput.ko" ;;
    *.ko) cp "$MODULE" "$ROOT/uinput.ko" ;;
    *) printf 'Expected an uncompressed or zstd-compressed module\n' >&2; exit 2 ;;
esac
cat > "$ROOT/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox mkdir -p /proc /sys /dev /run /tmp
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t tmpfs tmpfs /run
/bin/busybox insmod /uinput.ko
/usr/lib/systemd/systemd-udevd --daemon
/usr/bin/udevadm trigger --action=add
/usr/bin/udevadm settle -t 10
/test-pointer-libinput
echo "test exit status $?"
/bin/busybox poweroff -f
EOF
chmod 0755 "$ROOT/init"
(cd "$ROOT" && bsdtar --format newc -cf - .) | gzip -1 > "$OUT/initramfs.cpio.gz"
# A build root can prepare the VM without exposing host devices or QEMU.
if [[ ${PREPARE_ONLY:-0} == 1 ]]; then
    cp "$KERNEL" "$OUT/vmlinuz"
    printf 'Prepared diskless VM: %s\n' "$OUT"
    exit 0
fi
set +e
timeout --signal=TERM --kill-after=5 120 qemu-system-x86_64 \
    -accel kvm -cpu host -smp 2 -m 1024 -nodefaults -display none \
    -serial stdio -monitor none -no-reboot -kernel "$KERNEL" \
    -smbios 'type=1,manufacturer=Panasonic Corporation,product=CFFV1-3' \
    -initrd "$OUT/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 loglevel=4' > "$OUT/serial.log" 2>&1
result=$?
set -e
python3 - "$OUT/serial.log" <<'PY'
from pathlib import Path
import sys

log = Path(sys.argv[1]).read_text(errors='replace')
print(log)
unexpected = '\n'.join(line for line in log.splitlines()
                       if not line.startswith('EXPECTED pre-udev rejection:'))
if ('PASS pointer-libinput:' not in log or 'test exit status 0' not in log or 'Assertion' in log or
        'client bug' in unexpected or 'libinput bug' in unexpected):
    raise SystemExit(1)
PY
test "$result" = 0
