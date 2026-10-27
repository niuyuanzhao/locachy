#!/bin/bash
# Boot the built unified kernel image through OVMF as the firmware's fallback
# EFI/BOOT/BOOTX64.EFI, not -kernel. Only private image copies receive a test
# account/configuration; no host disk, ESP or network is exposed.
#   test-root-boot.sh LEVEL BUILT_OUTPUT_DIRECTORY NEW_TEST_DIRECTORY
# For another acceptance on the same image: ROOT_BOOT_DRIVER (a tools/ script
# driving the guest, test-root-boot.py by default), ROOT_BOOT_RC (a file of
# rc.conf lines added) and ROOT_BOOT_FILES (a folder copied into the user's
# Documents, the user's).
set -euo pipefail
export LC_ALL=C.UTF-8
if [ "$#" != 3 ]; then
    printf 'Usage: %s LEVEL BUILT_OUTPUT_DIRECTORY NEW_TEST_DIRECTORY\n' "$0" >&2
    exit 2
fi
level=$1
built=$(realpath "$2")
out=$(realpath -m "$3")
source=$(cd "$(dirname "$0")/.." && pwd)
case "$level" in
x86-64|x86-64-v2|x86-64-v3|x86-64-v4) ;;
*) printf 'Unsupported CPU level: %s\n' "$level" >&2; exit 2 ;;
esac
if [ "$level" != x86-64 ] && ! /usr/lib/ld-linux-x86-64.so.2 --help | grep -q "$level (supported"; then
    printf 'UNSUPPORTED %s: this host cannot execute that CPU level; no boot was attempted.\n' "$level" >&2
    exit 77
fi
for tool in qemu-system-x86_64 python3 unshare bsdtar mkfs.ext4 mkfs.fat sfdisk mmd mcopy openssl objcopy; do
    command -v "$tool" >/dev/null || { printf 'Missing test tool: %s\n' "$tool" >&2; exit 2; }
done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { printf 'KVM is not available\n' >&2; exit 2; }
[ ! -e "$out" ] || { printf 'Test directory already exists: %s\n' "$out" >&2; exit 2; }
case "$built$out" in *,*) printf 'QEMU image paths must not contain commas\n' >&2; exit 2 ;; esac
for file in root.tar.zst esp/locachy.efi; do
    [ -s "$built/$file" ] || { printf 'Missing build output: %s\n' "$built/$file" >&2; exit 2; }
done
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
[ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ] || { printf 'OVMF firmware is unavailable\n' >&2; exit 2; }
mkdir -p "$out"
# This fixed nonsecret password exists only in the throwaway test image.
hash=$(openssl passwd -6 -salt locachy-test Locachy123)
driver=${ROOT_BOOT_DRIVER:-test-root-boot.py}
[ -f "$source/tools/$driver" ] || { printf 'No such guest driver: %s\n' "$driver" >&2; exit 2; }
ROOT_BOOT_RC=${ROOT_BOOT_RC:+$(realpath "$ROOT_BOOT_RC")}
ROOT_BOOT_FILES=${ROOT_BOOT_FILES:+$(realpath "$ROOT_BOOT_FILES")}
# Only the test image logs to the serial port: the built image's own command
# line (in its .cmdline section), with the kernel's messages, and console=ttyS0.
objcopy -O binary --only-section=.cmdline "$built/esp/locachy.efi" "$out/cmdline.bin"
test_cmdline="$(tr -d '\0' <"$out/cmdline.bin" | sed -e 's/quiet loglevel=0/loglevel=7/') console=ttyS0"
grep -q 'net.ifnames=0' <<<"$test_cmdline" || { printf 'The built image has no LOCachy command line\n' >&2; exit 2; }
export out built hash ROOT_BOOT_RC ROOT_BOOT_FILES test_cmdline
unshare --map-auto --map-root-user --setuid 0 --setgid 0 bash -euc '
    root=$out/root
    mkdir "$root"
    bsdtar --numeric-owner --acls --xattrs -xpf "$built/root.tar.zst" -C "$root"
    # The image under test is made again by ukify in the root, as
    # locachy-boot makes it, with the serial command line.
    kernel=$(chroot "$root" pacman -Qlq linux-cachyos-bore | grep "/vmlinuz$")
    set --
    for u in "$root"/boot/*-ucode.img; do
        [ ! -f "$u" ] || set -- "$@" --initrd="${u#"$root"}"
    done
    chroot "$root" ukify build --linux="$kernel" "$@" --initrd=/usr/local/lib/locachy/boot/initramfs.img \
        --cmdline="$test_cmdline" --output=/tmp/boot-test.efi >"$out/ukify.log" 2>&1
    mv "$root/tmp/boot-test.efi" "$out/boot-test.efi"
    user=$(chroot "$root" getent passwd 1000 | cut -d: -f1 || true)
    if [ -n "$user" ]; then
        chroot "$root" usermod -p "$hash" "$user"
    else
        chroot "$root" useradd --uid 1000 --create-home --shell /bin/bash --password "$hash" boot-test
    fi
    # Containers and test configuration are private to this image. Keep the
    # normal lock/PAM programs; never set login_skip or bypass authentication.
    rm -rf "$root/etc/locachy/containers"
    mkdir -p "$root/etc/locachy/containers" "$root/usr/local/lib/locachy"
    printf "%s\n" "#!/bin/sh" "printf '\''<6>boot-test: service-ready\\n'\'' >/dev/kmsg" "exec sleep infinity" \
        >"$root/usr/local/lib/locachy/boot-test-service"
    chmod 755 "$root/usr/local/lib/locachy/boot-test-service"
    cat >"$root/etc/locachy/rc.conf" <<EOF
hibernate_after="0"
lock_after="0"
ime_chinese="0"
service_udev="/usr/local/lib/locachy/udev-client"
service_boot_test="/usr/local/lib/locachy/boot-test-service"
program_term="/usr/local/bin/term"
program_settings="/usr/local/bin/settings"
EOF
    [ -z "$ROOT_BOOT_RC" ] || cat "$ROOT_BOOT_RC" >>"$root/etc/locachy/rc.conf"
    if [ -n "$ROOT_BOOT_FILES" ]; then
        home=$(chroot "$root" getent passwd 1000 | cut -d: -f6)
        mkdir -p "$root$home/Documents"
        cp -R "$ROOT_BOOT_FILES"/. "$root$home/Documents/"
        chown -R 1000:1000 "$root$home/Documents"
    fi
    used=$(du -sm "$root" | cut -f1)
    size=$((used * 5 / 4 + 1024))
    [ "$size" -ge 8192 ] || size=8192
    truncate -s "${size}M" "$out/data.img"
    mkfs.ext4 -q -F -L locachy-root -d "$root" "$out/data.img"
    rm -rf "$root"
'
# AHCI presents sd* devices, which the real init scans by filesystem labels.
truncate -s 128M "$out/esp.img"
printf 'label: gpt\nstart=2048,size=258048,type=uefi\n' | sfdisk -q "$out/esp.img"
mkfs.fat -F 32 -s 1 --offset 2048 "$out/esp.img" 129024 >"$out/mkfs-fat.log" 2>&1
esp="$out/esp.img@@1048576"
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/locachy
mcopy -i "$esp" "$out/boot-test.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$built/esp/locachy.efi" ::/EFI/locachy/locachy.efi
cp "$ovmf_vars" "$out/ovmf-vars.fd"
qemu=
cleanup() {
    if [ -n "$qemu" ]; then
        kill "$qemu" 2>/dev/null || true
        wait "$qemu" 2>/dev/null || true
    fi
}
trap cleanup EXIT
seconds=${ROOT_BOOT_TIMEOUT:-240}
# The root was built for LEVEL; host exposes only actually supported features.
# QEMU_CPU can narrow the model for a separate minimum-CPU acceptance run.
printf 'Booting %s with KVM CPU %s; logs and screenshots: %s\n' "$level" "${QEMU_CPU:-host}" "$out"
timeout --signal=TERM --kill-after=5 "$seconds" qemu-system-x86_64 \
    -accel kvm -machine q35 -cpu "${QEMU_CPU:-host}" -m 2048 -smp 2 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$ovmf_code" \
    -drive if=pflash,format=raw,unit=1,file="$out/ovmf-vars.fd" \
    -drive if=none,id=esp,file="$out/esp.img",format=raw,snapshot=on \
    -device ide-hd,drive=esp,bus=ide.0 \
    -drive if=none,id=data,file="$out/data.img",format=raw,snapshot=on \
    -device ide-hd,drive=data,bus=ide.1 \
    -nic none -vga std -display none -monitor none -no-reboot \
    -chardev "socket,id=com1,path=$out/com1.sock,server=on,wait=on" -serial chardev:com1 \
    -qmp "unix:$out/qmp.sock,server=on,wait=off" </dev/null >"$out/qemu.out" 2>"$out/qemu.err" &
qemu=$!
python3 -I "$source/tools/$driver" "$out" "$seconds"
status=0
wait "$qemu" || status=$?
qemu=
[ "$status" = 0 ] || { printf 'FAIL QEMU exited with status %s; see %s/qemu.err\n' "$status" "$out" >&2; exit 1; }
printf 'PASS %s (%s): OVMF, the unified kernel image, real PAM unlock, the driven flow and clean power-off\n' \
    "$level" "$driver"
