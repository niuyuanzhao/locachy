#!/usr/bin/env bash
# Boot a disposable, diskless VM; never load vkms on the host. The host's
# packaged Terminus 16/28 fonts exercise actual text and fractional scaling.
set -euo pipefail
if (( $# != 3 )); then
    printf 'Usage: %s KERNEL VKMS_MODULE OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi
KERNEL=$(realpath "$1")
MODULE=$(realpath "$2")
OUT=$(realpath -m "$3")
SOURCE=$(cd "$(dirname "$0")/.." && pwd)
test -r "$KERNEL"
test -r "$MODULE"
test -r /dev/kvm && test -w /dev/kvm
if [[ -e "$OUT" ]]; then
    printf 'Output directory already exists: %s\n' "$OUT" >&2
    exit 2
fi
mkdir -p "$OUT"
CC=${CC:-musl-gcc}
"$CC" -static -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -I "$SOURCE/base/include" -idirafter /usr/include \
    "$SOURCE/tools/test-screen-vkms.c" "$SOURCE/base/ui.c" "$SOURCE/base/panel.c" \
    "$SOURCE/base/status.c" "$SOURCE/base/rc.c" -o "$OUT/init"
case "$MODULE" in
    *.zst) zstd -d -q -c "$MODULE" > "$OUT/vkms.ko" ;;
    *.ko) cp "$MODULE" "$OUT/vkms.ko" ;;
    *) printf 'Expected an uncompressed or zstd-compressed module\n' >&2; exit 2 ;;
esac
python3 - "$OUT" <<'PY'
from pathlib import Path
import gzip
import stat
import sys

out = Path(sys.argv[1])
font = gzip.decompress(Path('/usr/share/kbd/consolefonts/ter-u16b.psf.gz').read_bytes())
entries = [
    ('init', (out / 'init').read_bytes(), stat.S_IFREG | 0o755, 0, 0),
    ('vkms.ko', (out / 'vkms.ko').read_bytes(), stat.S_IFREG | 0o644, 0, 0),
    ('font.psf', font, stat.S_IFREG | 0o644, 0, 0),
    ('dev', b'', stat.S_IFDIR | 0o755, 0, 0),
    ('dev/console', b'', stat.S_IFCHR | 0o600, 5, 1),
]
prefix = Path('usr/local/share/locachy/terminus')
for folder in ('usr', 'usr/local', 'usr/local/share', 'usr/local/share/locachy', str(prefix)):
    entries.append((folder, b'', stat.S_IFDIR | 0o755, 0, 0))
font28 = gzip.decompress(Path('/usr/share/kbd/consolefonts/ter-u28b.psf.gz').read_bytes())
entries.append((str(prefix / 'ter-u28b.psf'), font28, stat.S_IFREG | 0o644, 0, 0))
entries.append(('TRAILER!!!', b'', 0, 0, 0))
archive = bytearray()
for ino, (name, data, mode, major, minor) in enumerate(entries, 1):
    name = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0]
    archive.extend(b'070701' + b''.join(f'{x:08x}'.encode() for x in fields))
    archive.extend(name)
    archive.extend(bytes((-len(archive)) % 4))
    archive.extend(data)
    archive.extend(bytes((-len(archive)) % 4))
(out / 'initramfs.cpio.gz').write_bytes(gzip.compress(archive, mtime=0))
PY
set +e
timeout --signal=TERM --kill-after=5 90 qemu-system-x86_64 \
    -accel kvm -cpu host -smp 2 -m 1024 -nodefaults -display none \
    -serial stdio -monitor none -no-reboot -kernel "$KERNEL" \
    -initrd "$OUT/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 loglevel=4' > "$OUT/serial.log" 2>&1
result=$?
set -e
python3 - "$OUT/serial.log" <<'PY'
from pathlib import Path
import sys

log = Path(sys.argv[1]).read_text(errors='replace')
print(log)
if 'PASS screen-vkms:' not in log or 'FAIL line' in log:
    raise SystemExit(1)
PY
test "$result" = 0
