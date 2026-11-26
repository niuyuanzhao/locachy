#!/bin/bash
# make-installer.sh [--profile-only] WORK_DIRECTORY OUTPUT_DIRECTORY
# One generic x86-64 installation medium, composed from packaged archiso.
# Target CPU/repository choices belong to locachy-install, never this image.
# Its app presets come from LOCACHY_APPS_SRC (a checkout or export of
# locachy-apps), validated by link/locachy_apps.py. LOCACHY_APPS_REPO and
# LOCACHY_APPS_BRANCH override the update source, LOCACHY_APPS_COMMIT names
# the checkout's expected HEAD or the export's declared base.
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
src=$(cd "$(dirname "$0")" && pwd -P)
baseline=${ARCHISO_BASELINE:-/usr/share/archiso/configs/baseline}
mkarchiso=${MKARCHISO:-mkarchiso}

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
apps_src=${LOCACHY_APPS_SRC:-}
[ -n "$apps_src" ] && [ -d "$apps_src" ] ||
    die "the app presets are missing: LOCACHY_APPS_SRC must be a checkout or export of their repository (locachy-apps)"
why=$(python3 -I "$src/link/locachy_apps.py" validate "$apps_src" 2>&1) || die "the app presets in $apps_src: $why"
apps_repo=${LOCACHY_APPS_REPO:-} apps_branch=${LOCACHY_APPS_BRANCH:-}
apps_base_commit=${LOCACHY_APPS_COMMIT:-}
apps_source_kind=export
if [ -e "$apps_src/.git" ] || [ -L "$apps_src/.git" ]; then
    prefix=$(git -C "$apps_src" rev-parse --show-prefix 2>/dev/null) && [ -z "$prefix" ] ||
        die "invalid app presets checkout: $apps_src"
    apps_source_kind=checkout
    apps_repo=${apps_repo:-$(git -C "$apps_src" remote get-url origin 2>/dev/null || true)}
    apps_branch=${apps_branch:-$(git -C "$apps_src" symbolic-ref --short HEAD 2>/dev/null || true)}
    head=$(git -C "$apps_src" rev-parse --verify HEAD 2>/dev/null || true)
    [ -z "$apps_base_commit" ] || [ "$apps_base_commit" = "$head" ] ||
        die "LOCACHY_APPS_COMMIT does not match the app presets checkout HEAD"
    apps_base_commit=$head
fi
[[ -z "$apps_base_commit" || "$apps_base_commit" =~ ^[0-9a-f]{40}$ ]] || die "invalid app presets commit"
apps_branch=${apps_branch:-main}
if [ "$profile_only" = 0 ]; then
    command -v "$mkarchiso" >/dev/null || die "mkarchiso is not available"
fi

mkdir -m 755 "$work" "$out"
mkdir -m 755 "$work/cache"
cache=${PACMAN_CACHE:-$work/cache}
[ -d "$cache" ] && [ ! -L "$cache" ] || die "package cache is missing: $cache"
profile=$work/profile
mkdir "$profile"
cp -a "$baseline/." "$profile/"
root=$profile/airootfs
payload=$root/usr/local/src/locachy
mkdir -p "$payload" "$root/usr/local/bin" "$root/etc/locachy" "$root/etc/pacman.d" "$root/etc/NetworkManager/conf.d" "$root/etc/systemd/system/multi-user.target.wants"
for dir in base init progs edit link tools pkgbuilds aur-patches containers installer; do
    cp -a "$src/$dir" "$payload/$dir"
done
[ ! -d "$src/po" ] || cp -a "$src/po" "$payload/po"
[ ! -d "$src/machines" ] || cp -a "$src/machines" "$payload/machines"
for file in make-root.sh make.sh make-installer.sh packages rc.conf; do
    cp -a "$src/$file" "$payload/$file"
done
# The upstream profile remains the template. These are only image metadata
# overrides, not a copied archiso implementation or a replacement initramfs.
cat "$payload/installer/profiledef.append" >>"$profile/profiledef.sh"
python3 -I "$payload/installer/rescue-boot-entries.py" "$profile"
# archiso honors a nondefault profile CacheDir. Do not reuse a host cache
# which may contain a differently built package with the same name/version.
awk -v cache="$cache" '{print; if ($0 == "[options]") print "CacheDir = " cache}' \
    "$payload/installer/pacman.conf" >"$profile/pacman.conf"
{ cat "$baseline/packages.x86_64"; printf '\n'; cat "$payload/installer/packages.x86_64"; } |
    awk 'NF && $1 !~ /^#/ && !seen[$1]++ {print $1}' >"$profile/packages.x86_64"
install -m 644 "$payload/installer/pacman.conf" "$root/etc/pacman.conf"
install -m 644 "$payload/installer/mirrorlist" "$root/etc/pacman.d/mirrorlist"

# Populate the catalog from one validated read and label those same bytes.
catalog=$root/usr/local/share/locachy/apps-catalog
origin=$root/etc/locachy/source-origin.json
mkdir -p "$(dirname "$catalog")"
why=$(python3 -IB - "$payload/link/locachy_apps.py" "$apps_src" "$catalog" "$apps_repo" \
      "$apps_branch" "$apps_base_commit" "$apps_source_kind" "$src" "$origin" 2>&1 <<'PY'
import importlib.util, json, os, subprocess, sys
helper, source, catalog, apps_repo, apps_branch, base, kind, src, origin = sys.argv[1:]
spec = importlib.util.spec_from_file_location("locachy_apps", helper)
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)


def git(directory, *args):
    return subprocess.check_output(["git", "--no-replace-objects", "-C", directory, *args],
                                   stderr=subprocess.DEVNULL, timeout=5)


def matches_commit(fmt, presets):
    # git status is not evidence of which bytes are in this snapshot (e.g.
    # ignored or assume-unchanged files). Compare with the commit's blobs.
    expected = {b"FORMAT": fmt.encode(),
                **{f"apps/{name}.conf".encode(): data for name, data in presets.items()}}
    for entry in git(source, "ls-tree", "-rz", base, "--", "FORMAT", "apps").split(b"\0"):
        if not entry:
            continue
        header, path = entry.split(b"\t", 1)
        mode, _, oid = header.split()
        data = expected.pop(path, None)
        if data is None or mode not in (b"100644", b"100755"):
            return False
        blob = git(source, "cat-file", "blob", oid.decode())
        if (blob.removesuffix(b"\n") if path == b"FORMAT" else blob) != data:
            return False
    return not expected


try:
    fmt, presets = apps.validate_tree(source)
    apps_commit = ""
    if kind == "checkout":
        dirty = bool(git(source, "status", "--porcelain", "--untracked-files=all"))
        if base and matches_commit(fmt, presets) and not dirty:
            apps_commit = base
        else:
            kind = "dirty-checkout" if base else "uncommitted-checkout"
    # A false match on commit would make the normal catalog updater skip it.
    apps.write_catalog(presets, catalog, apps.new_meta(fmt, "medium", apps_repo, apps_branch, apps_commit))
except (apps.CatalogError, OSError, subprocess.SubprocessError) as error:
    sys.exit(str(error))


def git_out(*args):
    try:
        return git(src, *args).decode(errors="replace").strip()
    except (OSError, subprocess.SubprocessError):
        return ""


repo = os.environ.get("LOCACHY_REPO") or git_out("remote", "get-url", "origin")
branch = os.environ.get("LOCACHY_BRANCH") or git_out("symbolic-ref", "--short", "HEAD")
commit = os.environ.get("LOCACHY_COMMIT") or git_out("rev-parse", "HEAD")
# Exported build snapshots can declare the same fact without carrying .git.
changed = os.environ.get("LOCACHY_SOURCE_CHANGED") == "1" or bool(git_out("status", "--porcelain", "--", src))
with open(origin, "w", encoding="utf-8") as f:
    json.dump({"repo": repo, "branch": branch or "main", "commit": commit, "source_changed": changed,
               "apps_repo": apps_repo, "apps_branch": apps_branch, "apps_commit": apps_commit,
               "apps_base_commit": base, "apps_source_kind": kind}, f)
    f.write("\n")
PY
) || die "the app presets in $apps_src: $why"
mkdir -p "$root/usr/local/lib/locachy"
install -m 755 "$payload/installer/locachy-install" "$root/usr/local/bin/locachy-install"
install -m 755 "$payload/installer/locachy-rescue" "$root/usr/local/bin/locachy-rescue"
install -m 755 "$payload/installer/locachy-rescue-storage" "$root/usr/local/lib/locachy/rescue-storage"
install -m 755 "$payload/installer/rescue-link.py" "$root/usr/local/lib/locachy/rescue-link.py"
install -m 644 "$payload/installer/locachy-rescue.service" "$root/etc/systemd/system/locachy-rescue.service"
install -m 644 "$payload/installer/locachy-rescue-link.service" "$root/etc/systemd/system/locachy-rescue-link.service"
install -m 644 "$payload/installer/issue" "$root/etc/issue"
install -m 644 "$payload/installer/NetworkManager.conf" "$root/etc/NetworkManager/conf.d/20-locachy-installer.conf"
# Build the live programs from the same sources the ISO carries. Compile
# with relative include paths so absolute work paths may contain spaces.
(cd "$payload" && sh installer/make-live.sh . "$root")
mkdir -p "$root/etc/systemd/system/getty@tty1.service.d" "$root/root"
install -m 644 "$payload/installer/autologin.conf" "$root/etc/systemd/system/getty@tty1.service.d/autologin.conf"
install -m 644 "$payload/installer/bash_profile" "$root/root/.bash_profile"
install -m 644 "$payload/installer/locachy-clipboard.service" "$root/etc/systemd/system/locachy-clipboard.service"
mkdir -p "$root/etc/systemd/system/multi-user.target.wants"
ln -sfn ../locachy-clipboard.service "$root/etc/systemd/system/multi-user.target.wants/locachy-clipboard.service"
ln -sfn ../locachy-rescue.service "$root/etc/systemd/system/multi-user.target.wants/locachy-rescue.service"
# The rescue boot entry runs its own UI service on tty1, not an autologin shell.
printf '[Unit]\nConditionKernelCommandLine=!locachy.mode=rescue\n' \
    >"$root/etc/systemd/system/getty@tty1.service.d/rescue.conf"

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
for name in installer term rescue locachy-rescue; do
    printf '  ["/usr/local/bin/%s"]="0:0:755"\n' "$name" >>"$profile/profiledef.sh"
done
for name in rescue-storage rescue-link.py; do
    printf '  ["/usr/local/lib/locachy/%s"]="0:0:755"\n' "$name" >>"$profile/profiledef.sh"
done
printf '  ["/usr/local/lib/locachy/keytable"]="0:0:755"\n  ["/usr/local/lib/locachy/clipboard"]="0:0:755"\n' \
    >>"$profile/profiledef.sh"
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
# Keep the packaged builder intact. This small tracked patch only prevents
# random-seed writes on read-only optical media; drift in upstream stops here.
cp "$(command -v "$mkarchiso")" "$work/mkarchiso"
patch --batch --fuzz=0 -p1 -d "$work" <"$payload/installer/archiso-readonly-seed.patch"
"$work/mkarchiso" -w "$work/build" -o "$out" "$profile"
python3 -I - "$profile" "$out" "$payload" "$catalog" "$origin" <<'PY'
import hashlib, json, pathlib, sys, time
profile, out, payload, catalog, origin = map(pathlib.Path, sys.argv[1:])
images = sorted(out.glob('*.iso'))
if len(images) != 1:
    raise RuntimeError('expected exactly one generic installer ISO')
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
image = images[0]
checksum = digest(image)
(out / 'SHA256SUMS').write_text(checksum + '  ' + image.name + '\n')
inputs = {str(path.relative_to(payload)): digest(path) for path in sorted(payload.rglob('*')) if path.is_file()}
catalog_inputs = {path.name: digest(path) for path in sorted(catalog.iterdir())}
record = {'cpu_baseline': 'x86-64', 'target_built_on_install': True, 'iso': image.name,
          'sha256': checksum, 'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
          'packages': (profile / 'packages.x86_64').read_text().splitlines(), 'source_sha256': inputs,
          'source_origin': json.loads(origin.read_text(encoding='utf-8')), 'apps_catalog_sha256': catalog_inputs}
(out / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
PY
printf 'make-installer: ISO output: %s\n' "$out"
