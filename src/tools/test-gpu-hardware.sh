#!/bin/bash
# Device-free tests for GPU hardware assets:
# - hp-omen-17-ck1 machine profile & hp_omen_gpu DKMS source (compiled against kernel headers when available)
# - locachy-gpu-unlock (GitHub fetch/overlay/build/install/set/remove & drop-ins)
# - locachy-gpu-power (hp-omen-wmi modes, auto/AC mapping, nvidia-powerd wiring, remove)
set -euo pipefail

src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-}
cleanup=0
if [ -z "$out" ]; then
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-gpu-hw-XXXXXX")
    cleanup=1
else
    mkdir -p "$out"
fi
trap 'if [ "$cleanup" -eq 1 ]; then rm -rf "$out"; fi' EXIT

python3 -B - "$src" "$out" <<'PY'
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

src = Path(sys.argv[1])
out = Path(sys.argv[2])

def load_script(path, mod_name):
    loader = importlib.machinery.SourceFileLoader(mod_name, str(path))
    spec = importlib.util.spec_from_loader(mod_name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod

# 1. Machine profile & installer discovery
profile = src / "machines/hp-omen-17-ck1.conf"
assert profile.is_file(), "hp-omen-17-ck1.conf missing"
ptxt = profile.read_text(encoding="utf-8")
for needle in (
    "# Brand: HP",
    'description="HP OMEN 17-ck1xxx',
    'display_card="xe"',
    'extdisplay_card="xe"',
    "i915.force_probe=!4688",
    "xe.force_probe=4688",
    'gpu_power_method="hp-omen-wmi"',
    'gpu_power_mode="full"',
    'nvidia_powerd="1"',
):
    assert needle in ptxt, f"missing {needle!r} in hp-omen-17-ck1.conf"

install_mod = load_script(src / "link/locachy-install", "locachy_install_mod")
install_mod.SOURCE = src
names = [n for n, _ in install_mod.machine_profiles()]
assert "hp-omen-17-ck1" in names, f"hp-omen-17-ck1 not discovered: {names}"
print("PASS hp-omen-17-ck1.conf syntax and installer discovery")

# 2. locachy-gpu-unlock PCI ID overlay idempotency (v2.0.0 recipes.c + legacy main.c)
unlock = load_script(src / "link/locachy-gpu-unlock", "locachy_gpu_unlock_mod")
# Fixture repository only: no locachy-net namespace in the test.
unlock.netns_prefix = lambda: []
meta = json.loads((src / "link/gpu-unlock-upstream.json").read_text(encoding="utf-8"))
assert meta["repository"] == "https://github.com/Zagitta/gpu-unlock.git"

repo_v2 = out / "repo-v2"
(repo_v2 / "kernel/gpu_unlock").mkdir(parents=True)
recipes = repo_v2 / "kernel/gpu_unlock/recipes.c"
recipes.write_text(
    "static const struct gpu_recipe recipes[] = {\n"
    "\tGA103(0x2414),\n"
    "\tGA104(0x2482),\n"
    "};\n",
    encoding="utf-8",
)
added = unlock.apply_extra_pci_ids(repo_v2, meta)
assert added == ["GA103(0x2460)"], f"unexpected added: {added}"
rtxt = recipes.read_text(encoding="utf-8")
assert "GA103(0x2414),\n\tGA103(0x2460)," in rtxt
assert unlock.apply_extra_pci_ids(repo_v2, meta) == [], "overlay must be idempotent"
assert rtxt.count("0x2460") == 1

repo_legacy = out / "repo-legacy"
(repo_legacy / "kernel/gpu_unlock").mkdir(parents=True)
legacy_main = repo_legacy / "kernel/gpu_unlock/main.c"
legacy_main.write_text("static const u16 ids[] = {\n\t0x2414,\n};\n", encoding="utf-8")
assert unlock.apply_extra_pci_ids(repo_legacy, meta) == ["0x2460"]
assert unlock.apply_extra_pci_ids(repo_legacy, meta) == []
print("PASS locachy-gpu-unlock PCI ID overlay (recipes.c and legacy main.c, idempotent)")

# 3. End-to-end mocked locachy-gpu-unlock install / status / set / remove
root = out / "unlock-root"
(root / "etc/locachy").mkdir(parents=True)
(root / "etc/locachy/packages").write_text("base\nlinux-cachyos\n", encoding="utf-8")
(root / "var/lib/pacman/local").mkdir(parents=True)
sys_pci = out / "sys-pci"
dev = sys_pci / "0000:01:00.0"
dev.mkdir(parents=True)
(dev / "vendor").write_text("0x10de\n")
(dev / "device").write_text("0x2460\n")
(dev / "class").write_text("0x030000\n")
(dev / "driver").symlink_to("/sys/bus/pci/drivers/nvidia")

fake_bin = out / "fake-bin"
fake_bin.mkdir(parents=True)
call_log = out / "unlock-calls.log"

def write_exec(path, body):
    path.write_text(body, encoding="utf-8")
    os.chmod(path, 0o755)

write_exec(
    fake_bin / "git",
    f"""#!/bin/sh
set -eu
printf 'git %s\\n' "$*" >>"{call_log}"
dest=""
for a in "$@"; do dest="$a"; done
mkdir -p "$dest/kernel/gpu_unlock" "$dest/packaging/arch"
cat >"$dest/kernel/gpu_unlock/recipes.c" <<'EOF'
static const struct gpu_recipe recipes[] = {{
\tGA103(0x2414),
}};
EOF
cat >"$dest/packaging/arch/PKGBUILD" <<'EOF'
pkgname=(gpu-unlock-dkms gpu-unlock-firmware)
pkgver=2.0.0
EOF
""",
)

write_exec(
    fake_bin / "makepkg",
    f"""#!/bin/sh
set -eu
printf 'makepkg %s\\n' "$*" >>"{call_log}"
grep -q 'GA103(0x2460)' ../../kernel/gpu_unlock/recipes.c
: > gpu-unlock-dkms-2.0.0-1-x86_64.pkg.tar.zst
: > gpu-unlock-firmware-2.0.0-1-x86_64.pkg.tar.zst
""",
)

write_exec(
    fake_bin / "pacman",
    f"""#!/bin/sh
set -eu
printf 'pacman %s\\n' "$*" >>"{call_log}"
db="{root}/var/lib/pacman/local"
if [ "$1" = "-U" ]; then
    for pkg in gpu-unlock-dkms gpu-unlock-firmware; do
        mkdir -p "$db/$pkg-2.0.0-1"
        printf '%%NAME%%\\n%s\\n\\n%%VERSION%%\\n2.0.0-1\\n' "$pkg" >"$db/$pkg-2.0.0-1/desc"
    done
elif [ "$1" = "-Rns" ]; then
    rm -rf "$db"/gpu-unlock-*
fi
""",
)

write_exec(
    fake_bin / "locachy-pkg",
    f"""#!/bin/sh
set -eu
printf 'locachy-pkg %s\\n' "$*" >>"{call_log}"
""",
)

write_exec(
    fake_bin / "locachy-boot",
    f"""#!/bin/sh
set -eu
printf 'locachy-boot %s\\n' "$*" >>"{call_log}"
""",
)

uenv = dict(os.environ)
uenv.update({
    "PATH": f"{fake_bin}:{os.environ.get('PATH', '')}",
    "LOCACHY_GPU_UNLOCK_ROOT": str(root),
    "LOCACHY_GPU_UNLOCK_META": str(src / "link/gpu-unlock-upstream.json"),
    "LOCACHY_GPU_UNLOCK_SYS_PCI": str(sys_pci),
    "LOCACHY_GPU_UNLOCK_SYS_MODULE": str(out / "no-module"),
    "LOCACHY_GPU_UNLOCK_BOOT": str(fake_bin / "locachy-boot"),
    "LOCACHY_GPU_UNLOCK_PKG": str(fake_bin / "locachy-pkg"),
    "LOCACHY_GPU_UNLOCK_NO_NETNS": "1",
    "LOCACHY_MACHINES_DIR": str(src / "machines"),
})

def run_unlock(*args, expect_ok=True):
    r = subprocess.run([sys.executable, "-B", str(src / "link/locachy-gpu-unlock"), *args],
                       env=uenv, capture_output=True, text=True)
    if expect_ok:
        assert r.returncode == 0, f"locachy-gpu-unlock {args} failed: {r.stderr}\n{r.stdout}"
    else:
        assert r.returncode != 0, f"locachy-gpu-unlock {args} unexpectedly succeeded"
    return r

st0 = json.loads(run_unlock("status", "--json").stdout)
assert st0["installed"] is False and st0["configured_targets"] == "off"
assert len(st0["nvidia_gpus"]) == 1 and st0["nvidia_gpus"][0]["device"] == "0x2460"

run_unlock("install", "--target", "all")
st1 = json.loads(run_unlock("status", "--json").stdout)
assert st1["installed"] is True and st1["dkms_version"] == "2.0.0-1"
assert st1["configured_targets"] == "all"
assert st1["modload_configured"] is True and st1["modprobe_configured"] is True
ml = (root / "etc/modules-load.d/50-locachy-gpu-unlock.conf").read_text()
mp = (root / "etc/modprobe.d/50-locachy-gpu-unlock.conf").read_text()
assert "gpu_unlock" in ml
assert "softdep nvidia pre: gpu_unlock" in mp
assert "options nvidia NVreg_DynamicPowerManagement=0x00" in mp
mf = (root / "etc/locachy/packages").read_text()
assert "gpu-unlock-dkms" in mf and "gpu-unlock-firmware" in mf
assert 'gpu_unlock_targets="all"' in (root / "etc/locachy/rc.conf").read_text()

run_unlock("set", "0000:01:00.0")
assert 'gpu_unlock_targets="0000:01:00.0"' in (root / "etc/locachy/rc.conf").read_text()
run_unlock("set", "bad;target", expect_ok=False)
run_unlock("set", "off")
assert "gpu_unlock_targets=" not in (root / "etc/locachy/rc.conf").read_text()
assert not (root / "etc/modules-load.d/50-locachy-gpu-unlock.conf").exists()
assert not (root / "etc/modprobe.d/50-locachy-gpu-unlock.conf").exists()

run_unlock("set", "all")
run_unlock("remove")
st2 = json.loads(run_unlock("status", "--json").stdout)
assert st2["installed"] is False and st2["configured_targets"] == "off"
mf2 = (root / "etc/locachy/packages").read_text()
assert "gpu-unlock-dkms" not in mf2 and "gpu-unlock-firmware" not in mf2
assert "base" in mf2 and "linux-cachyos" in mf2
calls = call_log.read_text()
assert "locachy-pkg kernel headers on" in calls
assert "locachy-boot " in calls
print("PASS locachy-gpu-unlock install, overlay, drop-ins, manifest, set, and remove")

# 4. locachy-gpu-power tests (hp-omen-17-ck1 layering, modes, AC/auto, nvidia-powerd, remove)
proot = out / "power-root"
(proot / "etc/locachy").mkdir(parents=True)
(proot / "etc/locachy/rc.conf").write_text('machine="hp-omen-17-ck1"\n', encoding="utf-8")
ps_dir = out / "sys-ps"
(ps_dir / "AC0").mkdir(parents=True)
(ps_dir / "AC0/type").write_text("Mains\n")
(ps_dir / "AC0/online").write_text("1\n")

penv = dict(os.environ)
penv.update({
    "LOCACHY_GPU_POWER_ROOT": str(proot),
    "LOCACHY_MACHINES_DIR": str(src / "machines"),
    "LOCACHY_HP_OMEN_DRIVER_SRC": str(src / "machines/hp-omen-gpu"),
    "LOCACHY_GPU_POWER_DRY_RUN": "1",
    "LOCACHY_GPU_POWER_NO_SIGNAL": "1",
    "LOCACHY_GPU_POWER_SYS_PS": str(ps_dir),
    "LOCACHY_GPU_POWER_SYS_MOD": str(out / "no-hp-omen-mod"),
})

def run_power(*args, expect_ok=True):
    r = subprocess.run([sys.executable, "-B", str(src / "link/locachy-gpu-power"), *args],
                       env=penv, capture_output=True, text=True)
    if expect_ok:
        assert r.returncode == 0, f"locachy-gpu-power {args} failed: {r.stderr}\n{r.stdout}"
    else:
        assert r.returncode != 0, f"locachy-gpu-power {args} unexpectedly succeeded"
    return r

pst0 = json.loads(run_power("status", "--json").stdout)
assert pst0["method"] == "hp-omen-wmi" and pst0["mode"] == "full"
assert (pst0["ctgp"], pst0["ppab"], pst0["dstate"]) == (1, 1, 1)
assert pst0["nvidia_powerd"] is True and pst0["nvidia_powerd_available"] is False

run_power("apply")
dkms_dir = proot / "usr/src/hp-omen-gpu-1.0"
for fname in ("hp_omen_gpu.c", "Makefile", "dkms.conf"):
    assert (dkms_dir / fname).is_file(), f"missing {fname} in DKMS staging dir"
omp = (proot / "etc/modprobe.d/50-locachy-hp-omen-gpu.conf").read_text()
oml = (proot / "etc/modules-load.d/50-locachy-hp-omen-gpu.conf").read_text()
assert "hp_omen_gpu" in oml
assert "options hp_omen_gpu ctgp=1 ppab=1 dstate=1" in omp
assert "service_nvidia_powerd=" not in (proot / "etc/locachy/rc.conf").read_text()

# Install fake nvidia-powerd binary inside isolated root and toggle on/off.
(proot / "usr/bin").mkdir(parents=True)
write_exec(proot / "usr/bin/nvidia-powerd", "#!/bin/sh\nexit 0\n")
run_power("nvidia-powerd", "on")
rc_txt = (proot / "etc/locachy/rc.conf").read_text()
assert 'nvidia_powerd="1"' in rc_txt and 'service_nvidia_powerd="/usr/bin/nvidia-powerd"' in rc_txt
run_power("nvidia-powerd", "off")
rc_txt = (proot / "etc/locachy/rc.conf").read_text()
assert 'nvidia_powerd="0"' in rc_txt and "service_nvidia_powerd=" not in rc_txt

for mode, want in (("ctgp", "ctgp=1 ppab=0 dstate=1"),
                   ("stock", "ctgp=0 ppab=0 dstate=1"),
                   ("eco", "ctgp=0 ppab=0 dstate=2"),
                   ("full", "ctgp=1 ppab=1 dstate=1")):
    run_power("apply", mode)
    assert want in (proot / "etc/modprobe.d/50-locachy-hp-omen-gpu.conf").read_text()

# auto mode follows power_profile and AC status.
rc_file = proot / "etc/locachy/rc.conf"
for prof, ac, want in (
    ("performance", "1", "ctgp=1 ppab=1 dstate=1"),
    ("balanced", "1", "ctgp=1 ppab=0 dstate=1"),
    ("power", "1", "ctgp=0 ppab=0 dstate=2"),
    ("auto", "1", "ctgp=1 ppab=0 dstate=1"),
    ("auto", "0", "ctgp=0 ppab=0 dstate=1"),
):
    (ps_dir / "AC0/online").write_text(ac + "\n")
    rc_file.write_text(f'machine="hp-omen-17-ck1"\npower_profile="{prof}"\ngpu_power_mode="auto"\n')
    run_power("apply")
    got = (proot / "etc/modprobe.d/50-locachy-hp-omen-gpu.conf").read_text()
    assert want in got, f"auto ({prof}, ac={ac}): expected {want!r}, got {got!r}"

run_power("remove")
assert not dkms_dir.exists()
assert not (proot / "etc/modprobe.d/50-locachy-hp-omen-gpu.conf").exists()
assert not (proot / "etc/modules-load.d/50-locachy-hp-omen-gpu.conf").exists()
print("PASS locachy-gpu-power machine layering, modes, auto/AC mapping, nvidia-powerd, and remove")
PY

# 5. Compile hp_omen_gpu.c against real Linux kernel headers when available (no install, no load).
kbuild="/lib/modules/$(uname -r)/build"
if [ "$(uname -s)" = "Linux" ] && [ -f "$kbuild/Makefile" ]; then
    kmod_dir="$out/hp-omen-gpu-kbuild"
    rm -rf "$kmod_dir"
    mkdir -p "$kmod_dir"
    cp "$src/machines/hp-omen-gpu/"* "$kmod_dir/"
    make -C "$kbuild" M="$kmod_dir" modules >/dev/null
    test -f "$kmod_dir/hp_omen_gpu.ko"
    echo "PASS hp_omen_gpu.c compiles cleanly against Linux $(uname -r) headers"
else
    echo "SKIP hp_omen_gpu.c kernel build (headers not present at $kbuild)"
fi

echo "PASS test-gpu-hardware.sh: all checks passed."
