#!/bin/bash
# make-root.sh [--target-root DIR] [--work-dir DIR] [--cache-dir DIR] [--cpu CPU] [LEVEL]:
# make our system's root on this machine, for another:
# an Arch-family root installed by pacman (the distributions' packages as
# they are: kernel, all drivers and firmware, toolchain). A minimal base
# runs locachy-pkg to reconcile the complete manifest before our part is
# compiled there (make.sh). From this
# machine it takes only pacman, its Arch (and CachyOS, if any) keyring (the
# trust anchor, as pacstrap does), its mirror lists, and its package cache,
# read only.
# With --target-root, DIR is an empty mounted ext4 root labelled locachy-root;
# no disk is formatted, no root/ESP is selected, and no archive is made. Boot
# files stay in DIR/usr/local/lib/locachy/boot for the installer to commit.
# A root caller uses private mount/PID/cgroup namespaces; an unprivileged
# caller maps their /etc/subuid range, as pacstrap -N does.
#
# LEVEL, the CPU level the system's packages are for (x86-64, the default,
# x86-64-v2, -v3, -v4): link/locachy-repos stacks the repositories for it,
# the most optimised first, each falling back to the next: CachyOS's
# optimised builds (v3 and up; znver4 for a Zen 4/5 CPU), ALHP's (v2 and up),
# [cachyos] (the kernel), Arch's. WITH_REPOS (core,extra,multilib by
# default) chooses the sections.
# CPU, what the system's own builds are for (gcc's -march: the CPU model, as
# the installer finds it on the machine itself; LEVEL by default): makepkg's
# local and AUR packages, and LOCachy's programs.
# The packages' install scripts and the builds' tests run here, chrooted:
# this machine's CPU must run LEVEL's and CPU's code.
#
# Output, in out/LEVEL/:
#   root.tar.zst  the root (numeric owners, ACLs and file capabilities kept)
#   esp/          the ESP's EFI/locachy/: the unified kernel image locachy.efi,
#                 which the firmware starts as EFI/BOOT/BOOTX64.EFI (locachy-boot)
#   packages.txt  name, version and packager of every package
set -euo pipefail
export LC_ALL=C.UTF-8

die() {
    echo "make-root: $*" >&2
    exit 1
}

arguments=("$@")
target= work_arg= cache_arg= level= cpu=
while [ "$#" -gt 0 ]; do
    case $1 in
    --target-root|--work-dir|--cache-dir)
        [ "$#" -ge 2 ] || die "$1 needs an absolute directory"
        case $1 in
        --target-root) [ -z "$target" ] || die "--target-root was given twice"; target=$2 ;;
        --work-dir) [ -z "$work_arg" ] || die "--work-dir was given twice"; work_arg=$2 ;;
        --cache-dir) [ -z "$cache_arg" ] || die "--cache-dir was given twice"; cache_arg=$2 ;;
        esac
        shift 2 ;;
    --cpu)
        [ "$#" -ge 2 ] || die "--cpu needs a CPU name (gcc's -march)"
        [ -z "$cpu" ] || die "--cpu was given twice"; cpu=$2; shift 2 ;;
    --) shift; break ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$level" ] || die "only one CPU level is accepted"; level=$1; shift ;;
    esac
done
[ "$#" -eq 0 ] || die "unexpected arguments"
level=${level:-x86-64}
case $level in
x86-64|x86-64-v2|x86-64-v3|x86-64-v4) ;;
*) die "LEVEL is x86-64, x86-64-v2, x86-64-v3 or x86-64-v4" ;;
esac
cpu=${cpu:-$level}
[[ $cpu =~ ^[a-z0-9][a-z0-9._+-]*$ ]] || die "--cpu: not a CPU name: $cpu"
# Package scripts execute target code too. Refuse unsupported hardware before
# creating namespaces, touching output directories or downloading packages.
if [ "$level" != x86-64 ] && ! /usr/lib/ld-linux-x86-64.so.2 --help | grep -q "$level (supported"; then
    die "this machine's CPU cannot run $level code, which the packages' scripts need"
fi

# Canonical absolute paths reject symlinks in parents as well as the final
# component. realpath -m validates a not-yet-created work/cache directory.
canonical_directory() {
    [[ "$1" = /* && "$1" != / && "$1" != *$'\n'* ]] || die "directory must be absolute and not /: $1"
    [ "$(realpath -m -- "$1")" = "$1" ] || die "directory must be canonical without symlinks: $1"
    [ ! -e "$1" ] || [ -d "$1" ] || die "not a directory: $1"
}
for path in "$target" "$work_arg" "$cache_arg"; do
    [ -z "$path" ] || canonical_directory "$path"
done

here=$(cd "$(dirname "$0")" && pwd)
if [ -n "$target" ]; then
    [ "$(id -u)" = 0 ] || die "--target-root requires root"
    [ -d "$target" ] && mountpoint -q -- "$target" || die "target root must already be mounted"
    [ "$(findmnt -rn --mountpoint "$target" -o FSTYPE)" = ext4 ] || die "target root must be ext4"
    [ "$(findmnt -rn --mountpoint "$target" -o LABEL)" = locachy-root ] || die "target root must be labelled locachy-root"
    [ "$(stat -c %d "$target")" != "$(stat -c %d /)" ] || die "target is the running root filesystem"
    for entry in "$target"/* "$target"/.[!.]* "$target"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        case ${entry##*/} in
        lost+found) [ -d "$entry" ] && [ ! -L "$entry" ] || die "invalid lost+found in target"
                    [ -z "$(find "$entry" -mindepth 1 -maxdepth 1 -print -quit)" ] || die "target lost+found is not empty" ;;
        efi) [ -d "$entry" ] && [ ! -L "$entry" ] || die "invalid efi mountpoint in target"
             # The installer masks its confirmed ESP before invoking us.
             [ -z "$(find "$entry" -mindepth 1 -maxdepth 1 -print -quit)" ] || die "target efi must be masked and empty" ;;
        *) die "target root is not empty: $entry" ;;
        esac
    done
    root=$target
    # Build on the target disk, not the live environment's RAM filesystem.
    # Only these two reserved locations may be nested beneath the target.
    work=${work_arg:-$root/.locachy-installer}
    cache=${cache_arg:-$root/var/cache/pacman/pkg}
    out=$work/output
else
    work=${work_arg:-$here/build/$level}
    root=$work/root
    cache=${cache_arg:-$here/cache}
    out=$here/out/$level
fi
gnupg=$work/gnupg
for path in "$work" "$cache"; do canonical_directory "$path"; done
case "$work/" in "$cache/"*) die "work directory must be outside cache" ;; esac
case "$cache/" in "$work/"*) die "cache directory must be outside work" ;; esac
case "$here/" in "$work/"*) die "work directory must not contain the source tree" ;; esac
if [ -n "$target" ]; then
    case "$root/" in "$work/"*|"$cache/"*) die "target root must be outside work and cache" ;; esac
    case "$work/" in
    "$root/.locachy-installer/") ;;
    "$root/"*) die "target work must use the reserved .locachy-installer directory" ;;
    esac
    case "$cache/" in
    "$root/var/cache/pacman/pkg/") ;;
    "$root/"*) die "target cache must use the reserved var/cache/pacman/pkg directory" ;;
    esac
    # Never reset an existing work directory. The empty-root check happens
    # before either reserved directory is created, including namespace reexec.
    [ ! -e "$work" ] || [ -z "$(find "$work" -mindepth 1 -maxdepth 1 -print -quit)" ] || die "target work directory must be empty"
else
    case "$cache/" in "$root/"*) die "cache directory must be outside target root" ;; esac
    canonical_directory "$out"
    case "$cache/" in "$out/"*) die "cache directory must be outside output" ;; esac
    case "$out/" in "$cache/"*|"$work/"*) die "output directory must be outside cache and work" ;; esac
    case "$work/" in "$out/"*) die "work directory must be outside output" ;; esac
fi

if [ -z "${MAKE_ROOT_NS:-}" ]; then
    if [ "$(id -u)" = 0 ]; then
        # A live installer needs no subuid mapping or systemd. Refresh proc
        # and the cgroup mount together before using namespace-relative paths;
        # never replace a mount in the caller's original mount namespace.
        if [ -z "${MAKE_ROOT_CGROUP_MOUNT:-}" ]; then
            exec unshare --mount --propagation private --mount-proc \
                env MAKE_ROOT_CGROUP_MOUNT=1 "$0" "${arguments[@]}"
        fi
        mount -t cgroup2 cgroup2 /sys/fs/cgroup
        parent=$(cut -d: -f3 </proc/self/cgroup)
        group=$(mktemp -d "/sys/fs/cgroup$parent/locachy-build.XXXXXX")
        # Zero means the writing process, independent of inherited PID views.
        echo 0 >"$group/cgroup.procs"
        cleanup_group() {
            echo 0 >"/sys/fs/cgroup$parent/cgroup.procs" 2>/dev/null || true
            rmdir "$group" 2>/dev/null || true
        }
        trap cleanup_group EXIT
        unshare --fork --pid --kill-child --mount --mount-proc --cgroup \
            env MAKE_ROOT_NS=1 "$0" "${arguments[@]}"
        exit $?
    fi
    # The clipboard contract uses real freezer cgroups. A rootless build
    # receives its own delegated scope, never write access to global groups.
    group=$(cut -d: -f3 </proc/self/cgroup)
    if [ ! -w "/sys/fs/cgroup$group/cgroup.procs" ]; then
        [ -z "${MAKE_ROOT_DELEGATED:-}" ] || die "no writable delegated cgroup for the package tests"
        exec systemd-run --user --scope --quiet -p Delegate=yes \
            env MAKE_ROOT_DELEGATED=1 "$0" "${arguments[@]}"
    fi
    exec unshare --fork --pid --kill-child --mount --cgroup --map-auto --map-root-user \
        --setuid 0 --setgid 0 env MAKE_ROOT_NS=1 "$0" "${arguments[@]}"
fi
# Root this mount at the build scope, excluding all other cgroups. Package
# builds see it read-only; only trusted contract tests receive freezer access.
mount -t cgroup2 cgroup2 /sys/fs/cgroup

repos=$here/link/locachy-repos
cachy=$(python3 -I "$repos" cachyos-levels --level "$level" --cpu "$cpu") || die "locachy-repos failed"
alhp=
[ "$level" = x86-64 ] || alhp=1
# The sections chosen (rc.conf's repo_core, repo_extra, repo_multilib).
repo_choice() {
    local s
    for s in core extra multilib; do
        case ",${WITH_REPOS:-core,extra,multilib}," in
        *",$s,"*) ;;
        *) printf 'repo_%s="0"\n' "$s" ;;
        esac
    done
}
# Only enough to run the normal package tools. The complete leaf manifest
# is reconciled by locachy-pkg inside the new root, never classified here.
packages=(base base-devel git python curl pacman-contrib cachyos-keyring cachyos-mirrorlist)
for lv in $cachy; do packages+=("cachyos-$lv-mirrorlist"); done
# CachyOS signs its packages with one key, published by fingerprint: taken
# from this machine's CachyOS keyring if it has one, else from a keyserver;
# the system's keyring package then comes from CachyOS's repository.
cachyos_key=882DCFE48E2051D48E2562ABF3B607488DB35A47
cachyos_mirrors=https://raw.githubusercontent.com/CachyOS/CachyOS-PKGBUILDS/master/cachyos-mirrorlist/cachyos-mirrorlist
# ALHP's keyring (its maintainer's key, which certifies the buildbot's that
# signs the packages) is a tarball, pinned here by its checksum as the AUR's
# alhp-keyring package pins it.
alhp_keyring=alhp-keyring-20250509
alhp_b2sum=1d12ae599d9da533aff66304f3111a19a80c1d132c933488549dd7fc7964a660d58e4d143a88a85f16cb6ef8dc28bab980f8f1702bdf17609e60fa603084bf81

grep -q '^Server' /etc/pacman.d/mirrorlist || die "/etc/pacman.d/mirrorlist has no Arch mirror"

if [ -n "$target" ]; then
    echo "== $level: build mounted target $root (work: $work)"
else
    echo "== $level: start over in $work"
    ! findmnt -rn -R "$work" >/dev/null 2>&1 || die "work directory still contains mounts"
    ! findmnt -rn -R "$out" >/dev/null 2>&1 || die "output directory still contains mounts"
    rm -rf "$work" "$out"
fi
mkdir -p "$root" "$cache" "$out/esp" "$work/mirrors"
mkdir -m 700 "$gnupg"

# This build's mirror lists: this machine's (the installer ranks the live
# system's first), else the projects' own: CachyOS's is its mirrorlist
# package's source, every server of it (pacman goes on to the next when one
# fails), its levels' lists made from it as cachyos-rate-mirrors makes them.
# Only CachyOS's pacman knows $arch_v3 and $arch_v4.
cp /etc/pacman.d/mirrorlist "$work/mirrors/mirrorlist"
if grep -qs '^Server' /etc/pacman.d/cachyos-mirrorlist; then
    cp /etc/pacman.d/cachyos-mirrorlist "$work/mirrors/"
elif ! curl -fsSL --max-time 60 "$cachyos_mirrors" | grep '^Server' >"$work/mirrors/cachyos-mirrorlist"; then
    echo 'Server = https://cdn77.cachyos.org/repo/$arch/$repo' >"$work/mirrors/cachyos-mirrorlist"
fi
for lv in $cachy; do
    m=/etc/pacman.d/cachyos-$lv-mirrorlist
    if grep -qs '^Server' "$m"; then
        grep '^Server' "$m" | sed "s/\\\$arch_$lv/x86_64_$lv/"
    else
        sed -n 's|/\$arch/|/x86_64_'"$lv"'/|p' "$work/mirrors/cachyos-mirrorlist"
    fi >"$work/mirrors/cachyos-$lv-mirrorlist"
done
if grep -qs '^Server' /etc/pacman.d/alhp-mirrorlist; then
    grep '^Server' /etc/pacman.d/alhp-mirrorlist >"$work/mirrors/alhp-mirrorlist"
else
    python3 -I "$repos" alhp-mirrors >"$work/mirrors/alhp-mirrorlist"
fi
repo_choice >"$work/repos.rc"
python3 -I "$repos" config --level "$level" --cpu "$cpu" --mirrors "$work/mirrors" --rc "$work/repos.rc" \
    >"$work/pacman.conf" || die "locachy-repos failed"

echo "== keys: archlinux (this machine's keyring), CachyOS${alhp:+, ALHP}"
pacman-key --gpgdir "$gnupg" --init >/dev/null 2>&1
pacman-key --gpgdir "$gnupg" --populate archlinux 2>&1 | tail -1
if [ -s /usr/share/pacman/keyrings/cachyos.gpg ]; then
    pacman-key --gpgdir "$gnupg" --populate cachyos 2>&1 | tail -1
else
    # As CachyOS's cachyos-repo.sh: the key from a keyserver (by its full
    # fingerprint), signed locally. A keyserver fails now and then: the next.
    got=
    for ks in hkps://keyserver.ubuntu.com hkps://keys.openpgp.org hkps://keyserver.ubuntu.com; do
        if pacman-key --gpgdir "$gnupg" --keyserver "$ks" --recv-keys "$cachyos_key" >"$work/key.log" 2>&1; then
            got=1
            break
        fi
        sleep 5
    done
    [ -n "$got" ] || { tail -5 "$work/key.log"; die "CachyOS's key $cachyos_key: no keyserver gave it"; }
    pacman-key --gpgdir "$gnupg" --lsign-key "$cachyos_key" >/dev/null 2>&1
fi
if [ -n "$alhp" ]; then
    t=$work/$alhp_keyring.tar.gz
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o "$t" "https://f.alhp.dev/alhp-keyring/$alhp_keyring.tar.gz" ||
        die "ALHP's keyring: not fetched"
    echo "$alhp_b2sum  $t" | b2sum -c --quiet || die "$t: wrong checksum"
    mkdir -p "$work/keyrings"
    bsdtar -xf "$t" -C "$work/keyrings" --strip-components 1 "$alhp_keyring/alhp.gpg" "$alhp_keyring/alhp-trusted"
    pacman-key --gpgdir "$gnupg" --populate-from "$work/keyrings" --populate alhp 2>&1 | tail -1
fi

# The API file systems for the packages' scripts, which run chrooted in the
# root, as pacstrap -N makes them: a mount namespace of our own, so nothing
# here reaches this machine; device nodes bound from this machine's /dev (a
# user namespace cannot make them).
owned_mounts=()
teardown() {
    chroot "$root" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2>/dev/null || true
    chroot "$root" setpriv --reuid=locachy-builder --regid=locachy-builder --init-groups \
        gpgconf --homedir /var/lib/locachy/builder/home/.gnupg --kill all 2>/dev/null || true
    gpgconf --homedir "$gnupg" --kill all 2>/dev/null || true
    local failed=0 path i
    # Only detach mounts created by this invocation. In target mode the
    # caller owns the root/ESP mounts, which must stay intact on return.
    for ((i=${#owned_mounts[@]} - 1; i >= 0; i--)); do
        path=${owned_mounts[i]}
        if [ "$path" = "$root/sys" ]; then
            umount --lazy "$path" 2>/dev/null || failed=1
        else
            umount "$path" 2>/dev/null || failed=1
        fi
    done
    owned_mounts=()
    [ "$failed" = 0 ] || die "a build mount could not be detached; target contents left intact"
    if [ -z "$target" ]; then
        ! findmnt -rn -R "$root" >/dev/null || die "still mounted under $root"
        rm -rf "$root"/dev/* "$root"/run/* "$root"/tmp/*
    fi
    rm -f "$root/etc/resolv.conf" # Do not retain the build environment resolver.
    : >"$root/etc/resolv.conf"
    # Build child namespaces have ended; sockets are not portable inputs,
    # even if an agent left one behind. Never follow directory symlinks.
    for path in "$root/etc/pacman.d/gnupg" "$root/var/lib/locachy/builder/home"; do
        [ ! -d "$path" ] || find "$path" -type s -delete
    done
}
trap teardown EXIT
mount --bind "$root" "$root"
owned_mounts+=("$root")
mkdir -p "$root"/{dev,proc,sys,run,tmp,etc/pacman.d/hooks,var/lib/pacman,var/log,var/cache/pacman/pkg}
if [ -n "$target" ]; then
    mkdir -p "$root/efi"
    mount -t tmpfs -o mode=0755,nosuid,nodev,noexec installer-esp "$root/efi"
    owned_mounts+=("$root/efi")
fi
mount --bind "$cache" "$root/var/cache/pacman/pkg"
owned_mounts+=("$root/var/cache/pacman/pkg")
mount -t proc proc "$root/proc" -o nosuid,noexec,nodev
owned_mounts+=("$root/proc")
mount -o rbind,ro=recursive /sys "$root/sys"
owned_mounts+=("$root/sys")
for d in full null random tty urandom zero; do
    touch "$root/dev/$d"
    mount --bind "/dev/$d" "$root/dev/$d"
    owned_mounts+=("$root/dev/$d")
done
ln -s /proc/self/fd "$root/dev/fd"
mkdir -p "$root/dev/shm"
mount -t tmpfs shm "$root/dev/shm" -o mode=1777,nosuid,nodev
owned_mounts+=("$root/dev/shm")
mount -t tmpfs run "$root/run" -o nosuid,nodev,mode=0755
owned_mounts+=("$root/run")
mount -t tmpfs tmp "$root/tmp" -o mode=1777,strictatime,nodev,nosuid
owned_mounts+=("$root/tmp")

echo "== install: ${packages[*]}"
# The kernel package wants an initramfs generator; we make our own. Its
# whole output goes to pacman.log; the console gets one line per package
# fetched or installed, counted against the transaction's "Packages (N)".
pacman --root "$root" --dbpath "$root/var/lib/pacman" \
    --cachedir "$cache" --cachedir /var/cache/pacman/pkg \
    --gpgdir "$gnupg" --hookdir "$root/etc/pacman.d/hooks" --config "$work/pacman.conf" \
    --noconfirm --assume-installed initramfs -Sy "${packages[@]}" 2>&1 | tee "$work/pacman.log" |
    awk '/^Packages \([0-9]+\)/ { total = substr($2, 2, length($2) - 2) }
         total && $2 == "downloading..." { printf "(%d/%s) downloading %s\n", ++fetched, total, $1; fflush() }
         $1 == "installing" { sub(/\.\.\.$/, "", $2); printf "(%d/%s) installing %s\n", ++installed, total, $2; fflush() }' ||
    { tail -20 "$work/pacman.log"; die "pacman failed; the log is $work/pacman.log"; }
grep -iE '^(error|warning)' "$work/pacman.log" | sort | uniq -c | head -20 || true

# The builds' tests run what is built for CPU here: everything that code
# uses (gcc's feature macros for it) must be this machine's too.
echo "== CPU for the system's own builds: $cpu"
cpu_macros() {
    chroot "$root" gcc "-march=$1" -dM -E -x c /dev/null | sed -n 's/^#define \(__[A-Z0-9_]*__\) 1$/\1/p' | sort -u
}
need=$(cpu_macros "$cpu") || die "gcc has no CPU $cpu"
lacking=$(comm -23 <(printf '%s\n' "$need") <(cpu_macros native | sort -u) | tr '\n' ' ')
[ -z "${lacking// }" ] || die "this machine's CPU has not what code for $cpu uses: $lacking"

echo "== the system's repositories and keyring"
python3 -I "$repos" config --level "$level" --cpu "$cpu" --mirrors /etc/pacman.d --rc "$work/repos.rc" \
    >"$root/etc/pacman.conf" || die "locachy-repos failed"
cp /etc/pacman.d/mirrorlist "$root/etc/pacman.d/mirrorlist"
[ -z "$alhp" ] || cp "$work/mirrors/alhp-mirrorlist" "$root/etc/pacman.d/alhp-mirrorlist"
# CachyOS's lists as this machine ranked them, over its packages' own (with
# $arch_vN: the system's pacman is CachyOS's).
for f in cachyos-mirrorlist $(for lv in $cachy; do echo "cachyos-$lv-mirrorlist"; done) .locachy-ranked; do
    if [ "$f" = .locachy-ranked ] || grep -qs '^Server' "/etc/pacman.d/$f"; then
        [ ! -f "/etc/pacman.d/$f" ] || cp "/etc/pacman.d/$f" "$root/etc/pacman.d/$f"
    fi
done
# ALHP's keyring is not a package: its two files go where the keyring
# packages put theirs.
[ -z "$alhp" ] || cp "$work"/keyrings/alhp* "$root/usr/share/pacman/keyrings/"
chroot "$root" pacman-key --init >/dev/null 2>&1
chroot "$root" pacman-key --populate archlinux cachyos ${alhp:+alhp} 2>&1 | tail -1
# This machine's pacman wrote the database entries of the packages named
# above, and CachyOS's adds a field (%INSTALLED_DB%) that other pacmans warn
# about at every command. The system's own pacman writes them again, in its
# own format (it rewrites an entry when the install reason changes).
chroot "$root" pacman -D --asdeps "${packages[@]}" >/dev/null 2>&1
chroot "$root" pacman -D --asexplicit "${packages[@]}" >/dev/null 2>&1
w=$(chroot "$root" pacman -Q 2>&1 >/dev/null | wc -l)
[ "$w" -eq 0 ] || die "the system's pacman warns about its own database ($w lines)"
chroot "$root" pacman -Sl | cut -d' ' -f1 | uniq -c | tr -s ' ' | tr '\n' ';'
echo

echo "== sources and package helpers in /usr/local/src/locachy"
s=$root/usr/local/src/locachy
mkdir -p "$s"
cp -r "$here/base" "$here/init" "$here/progs" "$here/edit" "$here/link" "$here/tools" "$here/containers" \
    "$here/pkgbuilds" "$here/aur-patches" "$here/make.sh" "$here/make-root.sh" "$here/rc.conf" "$here/packages" "$s/"
[ ! -d "$here/po" ] || cp -r "$here/po" "$s/"
[ ! -d "$here/machines" ] || cp -r "$here/machines" "$s/"
if [ "${WITH_IME:-1}" = 0 ]; then
    sed '/^librime$/d; /^words$/d; /^rime-/d' "$s/packages" >"$s/packages.no-ime"
    mv "$s/packages.no-ime" "$s/packages"
fi
case "${WITH_IME:-0}" in
en)
    printf '\n# Input method (English completion)\nlibrime\nwords\n' >>"$s/packages"
    ;;
1|zh)
    printf '\n# Input method (Rime & Chinese dictionaries)\nlibrime\nwords\nrime-frost-git\nrime-pinyin-simp\nrime-wanxiang-gram-zh-hans\n' >>"$s/packages"
    ;;
esac
python3 -I - "$s" "${LOCACHY_INSTALLER:-0}" "${WITH_FEATURES-__DEFAULT__}" "${WITH_EXTRA_PACKAGES:-}" <<'PY'
import json, sys
from pathlib import Path

src = Path(sys.argv[1])
from_installer = sys.argv[2] == "1"
raw_features = sys.argv[3]
raw_extra = sys.argv[4]

mods = json.loads((src / "link/modules.json").read_text(encoding="utf-8"))
if raw_features == "__DEFAULT__":
    chosen = [] if from_installer else [k for k, v in mods.items() if v.get("default_standalone")]
else:
    chosen = [x.strip() for x in raw_features.replace(" ", ",").split(",") if x.strip()]

for m in chosen:
    if m not in mods:
        raise SystemExit(f"make-root: unknown module in WITH_FEATURES: {m}")

pkg_file = src / "packages"
existing = {
    line.split("#", 1)[0].strip()
    for line in pkg_file.read_text(encoding="utf-8").splitlines()
    if line.split("#", 1)[0].strip()
}
added = []
for m in chosen:
    for p in mods[m].get("packages", []):
        if p not in existing:
            added.append(p)
            existing.add(p)

extras = [x.strip() for x in raw_extra.replace(",", " ").split() if x.strip()]
if not from_installer and raw_features == "__DEFAULT__" and "desktop" in chosen and "foot" not in existing:
    extras.append("foot")
for p in extras:
    if p not in existing:
        added.append(p)
        existing.add(p)

if added:
    with pkg_file.open("a", encoding="utf-8") as f:
        f.write("\n# Selected optional modules\n")
        for p in added:
            f.write(p + "\n")

rc_file = src / "rc.conf"
rc_lines = rc_file.read_text(encoding="utf-8").splitlines()
for m in chosen:
    for k, v in (mods[m].get("rc") or {}).items():
        rc_lines = [ln for ln in rc_lines if not ln.startswith(k + "=")]
        rc_lines.append(f'{k}="{v}"')
rc_file.write_text("\n".join(rc_lines) + "\n", encoding="utf-8")
PY
if [ -n "$cachy" ]; then
    printf '\n# Mirror lists of the CachyOS levels used\n' >>"$s/packages"
    for lv in $cachy; do echo "cachyos-$lv-mirrorlist" >>"$s/packages"; done
fi
# The repositories' sections chosen, as rc.conf says them to settings.
repo_choice >>"$s/rc.conf"
# makepkg reads these drop-ins after the packaged flags. Replace only the
# architecture choice (CPU: a CPU model, or the level, then tuned generic),
# retaining security, ABI, optimization and linker flags; Rust's the same,
# Go's (levels only) the level.
tune=
[ "$cpu" != "$level" ] || tune=" -mtune=generic"
mkdir -p "$root/etc/makepkg.conf.d"
cat >"$root/etc/makepkg.conf.d/99-locachy-cpu.conf" <<EOF
# LOCachy local builds target $cpu (CPU level $level).
_locachy_cpu_flags() {
    local flag
    for flag in \$1; do
        case \$flag in -march=*|-mtune=*) ;; *) printf '%s ' "\$flag" ;; esac
    done
    printf '%s' '-march=$cpu$tune'
}
CFLAGS=\$(_locachy_cpu_flags "\${CFLAGS:-}")
CXXFLAGS=\$(_locachy_cpu_flags "\${CXXFLAGS:-}")
unset -f _locachy_cpu_flags
RUSTFLAGS="\${RUSTFLAGS:+\$RUSTFLAGS }-C target-cpu=$cpu"
EOF
[ "$level" = x86-64 ] || echo "export GOAMD64=v${level#x86-64-v}" >>"$root/etc/makepkg.conf.d/99-locachy-cpu.conf"
cp -L /etc/resolv.conf "$root/etc/resolv.conf"
chroot "$root" sh /usr/local/src/locachy/link/install-pkg-tools /usr/local/src/locachy
# AUR builds need an unprivileged identity. The first run can bootstrap
# pikaur without either helper already installed; its dependencies stay in
# the disposable build root. No LOCachy package hooks are installed yet.
chroot "$root" /usr/local/bin/locachy-build -i pikaur-static
chroot "$root" /usr/local/bin/locachy-pkg sync
chroot "$root" /usr/local/bin/locachy-pkg status

echo "== our part: built against the complete package set"
chroot "$root" /usr/local/src/locachy/make.sh --boot "$level" "$cpu"
# The builder only stages boot files; it never writes the caller's ESP.
if [ -z "$target" ]; then
    cp "$root"/usr/local/lib/locachy/boot/locachy.efi "$out/esp/"
fi

echo "== result"
cp -f "$root/etc/locachy/packages" "$out/packages" 2>/dev/null || true
chroot "$root" pacman -Qi | awk -F' *: ' '/^Name/ {n = $2} /^Version/ {v = $2}
    /^Packager/ {print n, v, $2}' >"$out/packages.txt"
echo "packages: $(wc -l <"$out/packages.txt");" \
    "by packager: $(awk '{$1 = $2 = ""; print}' "$out/packages.txt" | sed 's/<.*//; s/^ *//' |
        sort | uniq -c | sort -rn | head -5 | tr -s ' ' | tr '\n' ';')"
v=$(ls "$root/usr/lib/modules")
echo "kernel $v: $(find "$root/usr/lib/modules/$v" -name '*.ko*' | wc -l) modules," \
    "modules.dep $(wc -l <"$root/usr/lib/modules/$v/modules.dep") lines"
if [ -n "$target" ]; then
    python3 -I - "$level" "$cpu" "$root/usr/local/lib/locachy/build.json" <<'PY'
import json, sys, time
level, cpu, path = sys.argv[1:]
with open(path, 'w') as f:
    json.dump({'level': level, 'cpu': cpu, 'mode': 'target-root', 'packages_verified': True,
               'boot_staged': True, 'built_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}, f, sort_keys=True)
    f.write('\n')
PY
fi
teardown
trap - EXIT
if [ -n "$target" ]; then
    echo "make-root: target built and verified: $root (boot files staged, ESP unchanged)"
    exit 0
fi
du -sh -x "$root" | cut -f1
bsdtar --numeric-owner --acls --xattrs -cf - -C "$root" . | zstd -q -T0 -10 -o "$out/root.tar.zst"
ls -l "$out/root.tar.zst" "$out/esp" | awk 'NF > 5 {print $5, $NF}'
