#!/bin/bash
# Real OVMF/KVM test of Maintenance Windows boot protection:
#   - Layer 1: sanitize-esp strips \EFI\BOOT\BOOTX64.EFI on a real FAT32 partition
#   - Layer 3: locachy-efi --bootnext-entry sets BootNext without polluting BootOrder
#   - Layer 4: locachy-efi --ensure-first --winmaint-guid heals a hijacked BootOrder
#   - Reclaim: locachy-efi --remove-entry removes the WinMaint NVRAM entry & BootNext
# The host runtime is mounted read-only; all writes stay inside NEW_OUTPUT.
set -euo pipefail
runtime=$(realpath "${1:?usage: test-winmaint-ovmf.sh RUNTIME_ROOT NEW_OUTPUT}")
out=$(realpath -m "${2:?}")
src=$(cd "$(dirname "$0")/.." && pwd)

kernel=${WINMAINT_VM_KERNEL:-/usr/lib/modules/$(uname -r)/vmlinuz}
[ -r "$kernel" ] || { echo "Host kernel not readable: $kernel (set WINMAINT_VM_KERNEL)" >&2; exit 2; }
modules=$(dirname "$kernel")/kernel
code=/usr/share/edk2/x64/OVMF_CODE.4m.fd
vars=/usr/share/edk2/x64/OVMF_VARS.4m.fd
[[ ! -e "$out" && -r /dev/kvm && -w /dev/kvm && -r "$code" && -r "$vars" ]] ||
    { echo "Needs /dev/kvm, OVMF under /usr/share/edk2/x64 and a new output directory" >&2; exit 2; }

mkdir -p "$out/initroot/bin" "$out/initroot/modules" "$out/results"
install -m755 /usr/bin/busybox "$out/initroot/bin/busybox"
for m in fs/netfs/netfs fs/9p/9p net/9p/9pnet net/9p/9pnet_virtio fs/overlayfs/overlay fs/fat/fat fs/fat/vfat; do
    if [ -r "$modules/$m.ko.zst" ]; then
        zstd -dq -c "$modules/$m.ko.zst" >"$out/initroot/modules/${m##*/}.ko"
    elif [ -r "$modules/$m.ko" ]; then
        cp "$modules/$m.ko" "$out/initroot/modules/${m##*/}.ko"
    else
        echo "Missing kernel module: $modules/$m.ko[.zst]" >&2
        exit 2
    fi
done

truncate -s 256M "$out/disk.img"
printf 'label: gpt\nstart=2048,size=258048,type=uefi,name="LOCACHY_EFI"\nstart=260096,size=258048,type=uefi,name="LOCACHY_WIN_ESP"\n' | sfdisk -q "$out/disk.img"
mkfs.fat -F32 -s1 --offset 2048 "$out/disk.img" 129024 >"$out/mkfs.log" 2>&1
mkfs.fat -F32 -s1 --offset 260096 "$out/disk.img" 129024 >>"$out/mkfs.log" 2>&1
cp "$vars" "$out/vars.fd"

cat >"$out/initroot/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin
b=/bin/busybox
$b mkdir -p /proc /sys /dev /lower /source /results /ram /target
$b mount -t proc proc /proc
$b mount -t sysfs sysfs /sys
$b mount -t devtmpfs devtmpfs /dev
for m in netfs 9pnet 9pnet_virtio 9p overlay fat vfat; do $b insmod /modules/$m.ko || exit 1; done
fail() { $b echo "WINMAINT_OVMF_BOOT_FAILED=$*"; $b poweroff -f; }
$b mount -t 9p -o trans=virtio,version=9p2000.L,ro,nosuid runtime /lower || fail runtime
$b mount -t 9p -o trans=virtio,version=9p2000.L,ro source /source || fail source
$b mount -t 9p -o trans=virtio,version=9p2000.L results /results || fail results
$b mount -t tmpfs -o size=512M tmpfs /ram || fail ram
$b mkdir /ram/upper /ram/work
$b mount -t overlay overlay -o nosuid,lowerdir=/lower,upperdir=/ram/upper,workdir=/ram/work /target || fail overlay
$b mkdir -p /target/proc /target/sys /target/dev /target/source /target/results /target/efi /target/run /target/tmp /target/mnt
for d in proc sys dev source results; do $b mount --bind /$d /target/$d; done
$b chroot /target /bin/bash /results/checks.sh
rc=$?
$b echo "WINMAINT_OVMF_STATUS=$rc"
$b echo "$rc" >/results/status
$b sync
$b poweroff -f
INIT
chmod 755 "$out/initroot/init"

cat >"$out/results/checks.sh" <<'CHECKS'
#!/bin/bash
set -euo pipefail
mkdir -p /sys/firmware/efi/efivars
mount -t efivarfs efivarfs /sys/firmware/efi/efivars
mount /dev/vda1 /efi
mkdir -p /efi/EFI/locachy /efi/EFI/BOOT
printf 'MZ locachy\n' >/efi/EFI/locachy/locachy.efi
cp /efi/EFI/locachy/locachy.efi /efi/EFI/BOOT/BOOTX64.EFI

mount /dev/vda2 /mnt
mkdir -p /mnt/EFI/Microsoft/Boot /mnt/EFI/BOOT
printf 'MZ win\n' >/mnt/EFI/Microsoft/Boot/bootmgfw.efi
printf 'MZ rogue-fallback\n' >/mnt/EFI/BOOT/BOOTX64.EFI
umount /mnt

install -m755 /source/link/locachy-efi /usr/local/bin/locachy-efi
install -m755 /source/link/locachy-winmaint /usr/local/bin/locachy-winmaint

python3 - <<'PY'
import importlib.machinery, importlib.util, json, subprocess

loader = importlib.machinery.SourceFileLoader("efi", "/usr/local/bin/locachy-efi")
spec = importlib.util.spec_from_loader(loader.name, loader)
efi = importlib.util.module_from_spec(spec)
loader.exec_module(efi)

def run(*args):
    p = subprocess.run(args, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(f"{args} failed ({p.returncode}): {p.stdout}{p.stderr}")
    return p.stdout

lsblk = json.loads(run(
    "lsblk", "--json", "--nodeps", "--paths", "--properties-by", "blkid",
    "--output", "PATH,PARTUUID", "/dev/vda2",
))
win_guid = lsblk["blockdevices"][0]["partuuid"].lower()
assert win_guid

out = run("/usr/local/bin/locachy-winmaint", "sanitize-esp", "--part", "/dev/vda2")
assert "BOOTX64.EFI" in out, out
print("PASS OVMF Layer-1 sanitize-esp stripped rogue BOOTX64.EFI on real FAT32 partition", flush=True)

run("/usr/local/bin/locachy-efi", "--first", "--esp", "/efi")
order, bootnext, every = efi.parse_entries(run("efibootmgr", "-v"))
loc = [e["number"] for e in every if e["label"] == efi.LABEL]
assert len(loc) == 1 and order[0] == loc[0]
print("PASS OVMF LOCachy registered first", flush=True)

run(
    "/usr/local/bin/locachy-efi", "--bootnext-entry",
    "--disk", "/dev/vda", "--part", "2", "--partuuid", win_guid,
    "--label", "LOCachy Maintenance Windows", "--loader", r"\EFI\Microsoft\Boot\bootmgfw.efi",
    "--esp", "/efi",
)
order, bootnext, every = efi.parse_entries(run("efibootmgr", "-v"))
win_nums = [e["number"] for e in every if e["guid"] == win_guid]
assert len(win_nums) == 1
assert bootnext == win_nums[0], (bootnext, win_nums)
assert win_nums[0] not in order, (win_nums, order)
assert order[0] == loc[0]
print("PASS OVMF --bootnext-entry set BootNext without touching BootOrder", flush=True)

run("efibootmgr", "--bootorder", ",".join([win_nums[0]] + order))
hijacked, _, _ = efi.parse_entries(run("efibootmgr", "-v"))
assert hijacked[0] == win_nums[0]

run("/usr/local/bin/locachy-efi", "--ensure-first", "--esp", "/efi", "--winmaint-guid", win_guid)
healed, _, _ = efi.parse_entries(run("efibootmgr", "-v"))
assert healed[0] == loc[0] and win_nums[0] not in healed, (healed, loc, win_nums)
print("PASS OVMF --ensure-first healed hijacked BootOrder", flush=True)

run("/usr/local/bin/locachy-efi", "--remove-entry", "--label", "LOCachy Maintenance Windows", "--partuuid", win_guid)
final_order, final_next, final_every = efi.parse_entries(run("efibootmgr", "-v"))
assert not [e for e in final_every if e["guid"] == win_guid]
assert final_next != win_nums[0]
assert final_order[0] == loc[0]
print("PASS OVMF --remove-entry cleaned WinMaint NVRAM entry and BootNext", flush=True)
PY
printf 'PASS WINMAINT OVMF complete\n'
CHECKS

python3 - "$out/initroot" "$out/initramfs.cpio.gz" <<'PY'
from pathlib import Path
import gzip, stat, sys

root = Path(sys.argv[1])
entries = [
    (str(p.relative_to(root)), p.read_bytes() if p.is_file() else b"",
     stat.S_IFREG | 0o755 if p.is_file() else stat.S_IFDIR | 0o755)
    for p in sorted(root.rglob("*"))
]
entries += [("dev", b"", stat.S_IFDIR | 0o755), ("dev/console", b"", stat.S_IFCHR | 0o600), ("TRAILER!!!", b"", 0)]
archive = bytearray()
for ino, (name, data, mode) in enumerate(entries, 1):
    major, minor = (5, 1) if name == "dev/console" else (0, 0)
    nb = name.encode() + b"\0"
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(nb), 0]
    archive += b"070701" + b"".join(f"{x:08x}".encode() for x in fields)
    archive += nb
    archive += bytes((-len(archive)) % 4)
    archive += data
    archive += bytes((-len(archive)) % 4)
Path(sys.argv[2]).write_bytes(gzip.compress(archive, mtime=0))
PY

status=0
timeout -k 10 120 qemu-system-x86_64 -accel kvm -machine q35 -cpu host -smp 2 -m 1024 \
    -nodefaults -display none -serial stdio -monitor none -no-reboot \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$code" \
    -drive "if=pflash,format=raw,unit=1,file=$out/vars.fd" \
    -kernel "$kernel" -initrd "$out/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 loglevel=4' \
    -drive "if=virtio,format=raw,file=$out/disk.img" \
    -virtfs "local,path=$runtime,mount_tag=runtime,security_model=none,readonly=on" \
    -virtfs "local,path=$src,mount_tag=source,security_model=none,readonly=on" \
    -virtfs "local,path=$out/results,mount_tag=results,security_model=none" >"$out/serial.log" 2>&1 || status=$?

python3 - "$out" "$status" <<'PY'
from pathlib import Path
import sys
out = Path(sys.argv[1])
text = (out / "serial.log").read_text(errors="replace")
print(text)
assert sys.argv[2] == "0" and (out / "results/status").read_text().strip() == "0"
assert "PASS WINMAINT OVMF complete" in text and "FAIL" not in text
PY
