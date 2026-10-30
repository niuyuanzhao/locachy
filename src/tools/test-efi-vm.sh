#!/bin/bash
# Real efibootmgr writes in private OVMF variables and a throwaway GPT ESP.
# The host runtime is read-only; packages are extracted only inside the guest.
set -euo pipefail
runtime=$(realpath "${1:?usage: test-efi-vm.sh RUNTIME_ROOT PACKAGE_CACHE NEW_OUTPUT}")
cache=$(realpath "${2:?}")
out=$(realpath -m "${3:?}")
src=$(cd "$(dirname "$0")/.." && pwd)
kernel=/usr/lib/modules/7.2.9-2-cachyos-server/vmlinuz
modules=/usr/lib/modules/7.2.9-2-cachyos-server/kernel
code=/usr/share/edk2/x64/OVMF_CODE.4m.fd
vars=/usr/share/edk2/x64/OVMF_VARS.4m.fd
[[ ! -e "$out" && -r /dev/kvm && -w /dev/kvm ]]
mkdir -p "$out/initroot/bin" "$out/initroot/modules" "$out/results"
install -m755 /usr/bin/busybox "$out/initroot/bin/busybox"
for module in fs/netfs/netfs fs/9p/9p net/9p/9pnet net/9p/9pnet_virtio fs/overlayfs/overlay fs/fat/fat fs/fat/vfat; do
    zstd -dq -c "$modules/$module.ko.zst" >"$out/initroot/modules/${module##*/}.ko"
done
truncate -s 128M "$out/esp.img"
printf 'label: gpt\nstart=2048,size=258048,type=uefi\n' | sfdisk -q "$out/esp.img"
mkfs.fat -F32 -s1 --offset 2048 "$out/esp.img" 129024 >"$out/mkfs.log" 2>&1
cp "$vars" "$out/vars.fd"
cat >"$out/initroot/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin
b=/bin/busybox
$b mkdir -p /proc /sys /dev /lower /source /results /cache /ram /target
$b mount -t proc proc /proc
$b mount -t sysfs sysfs /sys
$b mount -t devtmpfs devtmpfs /dev
for m in netfs 9pnet 9pnet_virtio 9p overlay fat vfat; do $b insmod /modules/$m.ko || exit 1; done
fail() { $b echo "EFI_VM_BOOT_FAILED=$*"; $b poweroff -f; }
$b mount -t 9p -o trans=virtio,version=9p2000.L,ro,nosuid runtime /lower || fail runtime
$b mount -t 9p -o trans=virtio,version=9p2000.L,ro source /source || fail source
$b mount -t 9p -o trans=virtio,version=9p2000.L,ro cache /cache || fail cache
$b mount -t 9p -o trans=virtio,version=9p2000.L results /results || fail results
$b mount -t tmpfs -o size=1G tmpfs /ram || fail ram
$b mkdir /ram/upper /ram/work
$b mount -t overlay overlay -o nosuid,lowerdir=/lower,upperdir=/ram/upper,workdir=/ram/work /target || fail overlay
$b mkdir -p /target/proc /target/sys /target/dev /target/source /target/results /target/cache /target/efi /target/run /target/tmp
$b mount --bind /proc /target/proc
$b mount --bind /sys /target/sys
$b mount --bind /dev /target/dev
$b mount --bind /source /target/source
$b mount --bind /results /target/results
$b mount --bind /cache /target/cache
$b chroot /target /bin/bash /results/checks.sh
result=$?
$b echo "EFI_VM_STATUS=$result"
$b echo "$result" >/results/status
$b sync
$b poweroff -f
INIT
cat >"$out/results/checks.sh" <<'CHECKS'
#!/bin/bash
set -euo pipefail
mkdir -p /sys/firmware/efi/efivars
mount -t efivarfs efivarfs /sys/firmware/efi/efivars
mount /dev/vda1 /efi
mkdir -p /efi/EFI/locachy /efi/EFI/BOOT
printf 'MZ test image\n' >/efi/EFI/locachy/locachy.efi
cp /efi/EFI/locachy/locachy.efi /efi/EFI/BOOT/BOOTX64.EFI
install -m755 /source/link/locachy-efi /usr/local/bin/locachy-efi
install -m755 /source/link/locachy-boot /usr/local/bin/locachy-boot
python3 - <<'PY'
import importlib.machinery, importlib.util, subprocess, json
loader=importlib.machinery.SourceFileLoader('efi','/usr/local/bin/locachy-efi')
spec=importlib.util.spec_from_loader(loader.name,loader)
efi=importlib.util.module_from_spec(spec);loader.exec_module(efi)
def run(*args):
    p=subprocess.run(args,text=True,capture_output=True)
    if p.returncode:
        print(p.stdout+p.stderr,flush=True)
        print(subprocess.run(['efibootmgr','-v'],text=True,capture_output=True).stdout,flush=True)
        raise RuntimeError(f'{args}: status {p.returncode}')
    return p.stdout
def state(): return efi.entries(run('efibootmgr','-v'))
def ours():
    _,part,guid=efi.esp_target('/efi')
    return [r for r in state()[1] if r['guid']==guid and r['part']==part and r['loader']==efi.LOADER.lower()]
old_order,_=state()
run('/usr/local/bin/locachy-efi','--first')
assert len(ours())==1
number=ours()[0]['number']
assert state()[0]==[number]+[n for n in old_order if n!=number]
print('PASS installer registration first; other boot entries retain their order',flush=True)
run('/usr/local/bin/locachy-boot','--register-only')
assert len(ours())==1 and ours()[0]['number']==number
print('PASS repeat registration reuses exact entry without duplicates',flush=True)
run('efibootmgr','--bootnum',number,'--inactive')
run('efibootmgr','--bootorder',','.join(n for n in old_order if n!=number))
run('/usr/local/bin/locachy-boot','--register-only')
assert ours()[0]['active'] and state()[0]==old_order+[number]
print('PASS inactive and missing BootOrder repaired without recreation',flush=True)
run('efibootmgr','--bootnum',number,'--delete-bootnum')
assert not ours()
run('/usr/local/bin/locachy-boot','--register-only')
assert len(ours())==1
print('PASS deleted entry recreated by installed-system register-only',flush=True)
# Preserve an intentional firmware order through a normal boot-image update.
order,_=state()
if len(order)>1: order=order[1:]+order[:1]
run('efibootmgr','--bootorder',','.join(order))
open('/results/order-before.json','w').write(json.dumps(order))
open('/results/entry-before.json','w').write(json.dumps(ours()))
open('/results/nvram-before.txt','w').write(run('efibootmgr','-v'))
PY
# The cached build root predates ukify; extract its cached package and pefile
# only into this disposable guest overlay, never into the runtime or host.
for package in systemd-ukify python-pefile; do
    file=$(find /cache -maxdepth 1 -name "$package-*.pkg.tar.zst" -print -quit)
    test -n "$file"
    bsdtar -xpf "$file" -C /
done
# The actual image update, not a stand-in helper invocation.
timeout 180 /usr/local/bin/locachy-boot >/results/image-update.log 2>&1
python3 - <<'PY'
from pathlib import Path
import importlib.machinery,importlib.util,json,subprocess
loader=importlib.machinery.SourceFileLoader('efi','/usr/local/bin/locachy-efi')
spec=importlib.util.spec_from_loader(loader.name,loader)
efi=importlib.util.module_from_spec(spec);loader.exec_module(efi)
text=subprocess.run(['efibootmgr','-v'],check=True,text=True,capture_output=True).stdout
Path('/results/nvram-after.txt').write_text(text)
order,entries=efi.entries(text)
assert order==json.loads(Path('/results/order-before.json').read_text())
assert entries==json.loads(Path('/results/entry-before.json').read_text())
a=Path('/efi/EFI/locachy/locachy.efi').read_bytes()
b=Path('/efi/EFI/BOOT/BOOTX64.EFI').read_bytes()
assert len(a)>1024*1024 and a==b
assert 'locachy-efi: Boot' in Path('/results/image-update.log').read_text()
print('PASS actual boot image rebuild updates ESP and preserves chosen BootOrder',flush=True)
PY
printf 'PASS EFI VM complete\n'
CHECKS
chmod 755 "$out/initroot/init"
python3 - "$out/initroot" "$out/initramfs.cpio.gz" <<'PY'
from pathlib import Path
import gzip,stat,sys
root=Path(sys.argv[1])
entries=[(str(p.relative_to(root)),p.read_bytes() if p.is_file() else b'',stat.S_IFREG|0o755 if p.is_file() else stat.S_IFDIR|0o755)
         for p in sorted(root.rglob('*'))]
entries += [('dev',b'',stat.S_IFDIR|0o755),('dev/console',b'',stat.S_IFCHR|0o600),('TRAILER!!!',b'',0)]
archive=bytearray()
for ino,(name,data,mode) in enumerate(entries,1):
 major,minor=(5,1) if name=='dev/console' else (0,0)
 name=name.encode()+b'\0'
 fields=[ino,mode,0,0,1,0,len(data),0,0,major,minor,len(name),0]
 archive+=b'070701'+b''.join(f'{x:08x}'.encode() for x in fields)
 archive+=name;archive+=bytes((-len(archive))%4);archive+=data;archive+=bytes((-len(archive))%4)
Path(sys.argv[2]).write_bytes(gzip.compress(archive,mtime=0))
PY
status=0
timeout -k 10 360 qemu-system-x86_64 -accel kvm -machine q35 -cpu host -smp 2 -m 2048 \
    -nodefaults -display none -serial stdio -monitor none -no-reboot \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$code" \
    -drive "if=pflash,format=raw,unit=1,file=$out/vars.fd" \
    -kernel "$kernel" -initrd "$out/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 loglevel=4' \
    -drive "if=virtio,format=raw,file=$out/esp.img" \
    -virtfs "local,path=$runtime,mount_tag=runtime,security_model=none,readonly=on" \
    -virtfs "local,path=$src,mount_tag=source,security_model=none,readonly=on" \
    -virtfs "local,path=$cache,mount_tag=cache,security_model=none,readonly=on" \
    -virtfs "local,path=$out/results,mount_tag=results,security_model=none" >"$out/serial.log" 2>&1 || status=$?
python3 - "$out" "$status" <<'PY'
from pathlib import Path
import sys
out=Path(sys.argv[1]);text=(out/'serial.log').read_text(errors='replace')
print(text)
assert sys.argv[2]=='0' and (out/'results/status').read_text().strip()=='0'
assert 'PASS EFI VM complete' in text and 'FAIL' not in text
PY
