#!/bin/bash
# make-installer.sh [--profile-only] WORK_DIRECTORY OUTPUT_DIRECTORY
# One generic x86-64 installation medium, composed from packaged archiso.
# Target CPU/repository choices belong to locachy-install, never this image.
set -euo pipefail
export LC_ALL=C.UTF-8

profile_only=0
if [ "${1:-}" = --profile-only ]; then
    profile_only=1
    shift
fi
[ "$#" = 2 ] || {
    echo "usage: make-installer.sh [--profile-only] WORK_DIRECTORY OUTPUT_DIRECTORY" >&2
    exit 2
}
src=$(cd "$(dirname "$0")" && pwd)
baseline=${ARCHISO_BASELINE:-/usr/share/archiso/configs/baseline}
mkarchiso=${MKARCHISO:-mkarchiso}
live_build=${LIVE_BUILD:-$src/installer/make-live.sh}

die() {
    echo "make-installer: $*" >&2
    exit 1
}

# Reject both destinations before creating either. Resolving the existing
# parent also catches aliases; neither a previous build nor its output is lost.
new_path() {
    local parent name
    [ ! -e "$1" ] && [ ! -L "$1" ] || die "destination exists: $1"
    parent=$(dirname "$1")
    name=$(basename "$1")
    [ -d "$parent" ] && [ "$name" != . ] && [ "$name" != .. ] || die "destination needs an existing parent: $1"
    printf '%s/%s\n' "$(cd "$parent" && pwd -P)" "$name"
}
work=$(new_path "$1")
out=$(new_path "$2")
[ "$work" != "$out" ] || die "work and output directories must differ"
for path in "$work" "$out"; do
    case "$path/" in "$src/"*) die "build destinations must be outside the source tree" ;; esac
done
case "$out/" in "$work/"*) die "output must not be inside work" ;; esac
case "$work/" in "$out/"*) die "work must not be inside output" ;; esac
[ -f "$baseline/profiledef.sh" ] && [ -f "$baseline/packages.x86_64" ] || die "packaged archiso baseline is missing: $baseline"
[ -f "$src/link/locachy-install" ] || die "source installer entry is missing: $src/link/locachy-install"
if [ "$profile_only" = 0 ]; then
    command -v "$mkarchiso" >/dev/null || die "mkarchiso is not available"
fi

mkdir -m 755 "$work" "$out"
mkdir -m 755 "$work/cache"
profile=$work/profile
mkdir "$profile"
cp -a "$baseline/." "$profile/"
# The upstream profile remains the template. These are only image metadata
# overrides, not a copied archiso implementation or a replacement initramfs.
cat "$src/installer/profiledef.append" >>"$profile/profiledef.sh"
# archiso honors a nondefault profile CacheDir. Do not reuse a host cache
# which may contain a differently built package with the same name/version.
awk -v cache="$work/cache" '{print; if ($0 == "[options]") print "CacheDir = " cache}' \
    "$src/installer/pacman.conf" >"$profile/pacman.conf"
{ cat "$baseline/packages.x86_64"; printf '\n'; cat "$src/installer/packages.x86_64"; } |
    awk 'NF && $1 !~ /^#/ && !seen[$1]++ {print $1}' >"$profile/packages.x86_64"

root=$profile/airootfs
payload=$root/usr/local/src/locachy
mkdir -p "$payload" "$root/usr/local/bin" "$root/etc/locachy" "$root/etc/pacman.d" "$root/etc/NetworkManager/conf.d" "$root/etc/systemd/system/multi-user.target.wants"
install -m 644 "$src/installer/pacman.conf" "$root/etc/pacman.conf"
install -m 644 "$src/installer/mirrorlist" "$root/etc/pacman.d/mirrorlist"
for dir in base init progs edit link tools pkgbuilds aur-patches containers installer; do
    cp -a "$src/$dir" "$payload/$dir"
done
[ ! -d "$src/po" ] || cp -a "$src/po" "$payload/po"
[ ! -d "$src/machines" ] || cp -a "$src/machines" "$payload/machines"
for file in make-root.sh make.sh make-installer.sh packages rc.conf; do
    cp -a "$src/$file" "$payload/$file"
done
python3 -I - "$src" "$root/etc/locachy/source-origin.json" <<'PY'
import json, os, subprocess, sys
src, dest = sys.argv[1], sys.argv[2]
def git_out(*args):
    try:
        return subprocess.check_output(["git", "-C", src, *args], stderr=subprocess.DEVNULL, text=True, timeout=5).strip()
    except Exception:
        return ""
repo = os.environ.get("LOCACHY_REPO") or git_out("remote", "get-url", "origin")
branch = os.environ.get("LOCACHY_BRANCH") or git_out("symbolic-ref", "--short", "HEAD")
commit = os.environ.get("LOCACHY_COMMIT") or git_out("rev-parse", "HEAD")
with open(dest, "w", encoding="utf-8") as f:
    json.dump({"repo": repo, "branch": branch or "main", "commit": commit}, f)
    f.write("\n")
PY
install -m 755 "$src/installer/locachy-install" "$root/usr/local/bin/locachy-install"
install -m 644 "$src/installer/issue" "$root/etc/issue"
install -m 644 "$src/installer/NetworkManager.conf" "$root/etc/NetworkManager/conf.d/20-locachy-installer.conf"
# The full-screen installer on tty1 (root logged in there, as archiso's
# releng does it; the serial console asks as before), and its terminal.
sh "$live_build" "$src" "$root"
mkdir -p "$root/etc/systemd/system/getty@tty1.service.d" "$root/root"
install -m 644 "$src/installer/autologin.conf" "$root/etc/systemd/system/getty@tty1.service.d/autologin.conf"
install -m 644 "$src/installer/bash_profile" "$root/root/.bash_profile"

# Only local console access is enabled by the live image. Do not carry the
# baseline's passwordless-root SSH service or cloud-config provisioning path.
rm -f "$root/etc/systemd/system/multi-user.target.wants/sshd.service"
rm -f "$root/etc/ssh/sshd_config.d/10-archiso.conf"
ln -sfn /dev/null "$root/etc/systemd/system/sshd.service"
ln -sfn /dev/null "$root/etc/systemd/system/cloud-init.target"
# NetworkManager manages wired and Wi-Fi links; resolved provides DNS.
rm -f "$root/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
rm -f "$root/etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service"
rm -f "$root/etc/systemd/system/sockets.target.wants/systemd-networkd.socket"
ln -sfn /dev/null "$root/etc/systemd/system/systemd-networkd.service"
ln -sfn /dev/null "$root/etc/systemd/system/systemd-networkd.socket"
ln -sfn /usr/lib/systemd/system/NetworkManager.service "$root/etc/systemd/system/multi-user.target.wants/NetworkManager.service"
ln -sfn /usr/lib/systemd/system/systemd-resolved.service "$root/etc/systemd/system/multi-user.target.wants/systemd-resolved.service"
ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"

# archiso copies overlay files as 0644 unless its profile specifies otherwise.
# Preserve executable source entry points without granting execute to data/code.
printf '\nfile_permissions+=(\n  ["/usr/local/bin/locachy-install"]="0:0:755"\n' >>"$profile/profiledef.sh"
printf '  ["/usr/local/bin/installer"]="0:0:755"\n  ["/usr/local/bin/term"]="0:0:755"\n' >>"$profile/profiledef.sh"
printf '  ["/usr/local/lib/locachy/keytable"]="0:0:755"\n' >>"$profile/profiledef.sh"
while IFS= read -r file; do
    IFS= read -r first <"$file" || true
    case "$first" in
    '#!'*)
        path=${file#"$root"}
        [[ "$path" != *'"'* && "$path" != *'$'* && "$path" != *'`'* && "$path" != *'\\'* ]] || die "unsupported executable source path: $path"
        printf '  ["%s"]="0:0:755"\n' "$path" >>"$profile/profiledef.sh"
        ;;
    esac
done < <(find "$payload" -type f | LC_ALL=C sort)
printf ')\n' >>"$profile/profiledef.sh"
printf 'make-installer: generic x86-64 profile: %s\n' "$profile"
if [ "$profile_only" = 1 ]; then
    exit 0
fi
"$mkarchiso" -w "$work/build" -o "$out" "$profile"
python3 -I - "$profile" "$out" <<'PY'
import hashlib, json, pathlib, sys, time
profile, out = map(pathlib.Path, sys.argv[1:])
images = sorted(out.glob('*.iso'))
if len(images) != 1:
    raise RuntimeError('expected exactly one generic installer ISO')
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
image = images[0]
checksum = digest(image)
(out / 'SHA256SUMS').write_text(checksum + '  ' + image.name + '\n')
inputs = {}
for path in sorted((profile / 'airootfs/usr/local/src/locachy').rglob('*')):
    if path.is_file():
        inputs[str(path.relative_to(profile / 'airootfs/usr/local/src/locachy'))] = digest(path)
record = {'cpu_baseline': 'x86-64', 'target_built_on_install': True, 'iso': image.name,
          'sha256': checksum, 'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
          'packages': (profile / 'packages.x86_64').read_text().splitlines(), 'source_sha256': inputs}
(out / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
PY
printf 'make-installer: ISO output: %s\n' "$out"
