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

# 4b. End-to-end mocked locachy-nvidia (open -> 580xx -> off, GSP drop-in, gpu-unlock sync, multilib guard)
nvroot = out / "nv-root"
(nvroot / "etc/locachy").mkdir(parents=True)
(nvroot / "etc/locachy/packages").write_text("base\nlinux-cachyos-bore\n", encoding="utf-8")
(nvroot / "etc/locachy/rc.conf").write_text(
    'boot_kernel="linux-cachyos-bore"\ngpu_unlock_targets="all"\n', encoding="utf-8"
)
nv_db = nvroot / "var/lib/pacman/local"
nv_db.mkdir(parents=True)
nv_bin = out / "nv-bin"
nv_bin.mkdir(parents=True)
nv_log = out / "nv-calls.log"
nv_ver_map = {
    "nvidia-open-dkms": "595.45.04-1",
    "nvidia-utils": "595.45.04-1",
    "nvidia-580xx-dkms": "580.126.25-1",
    "nvidia-580xx-utils": "580.126.25-1",
    "dkms": "3.1.5-1",
    "linux-cachyos-bore-headers": "6.19.1-1",
}

def db_install(pkg_name):
    ver = nv_ver_map.get(pkg_name, "1.0-1")
    d = nv_db / f"{pkg_name}-{ver}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "desc").write_text(f"%NAME%\n{pkg_name}\n\n%VERSION%\n{ver}\n", encoding="utf-8")

def db_remove(pkg_name):
    for d in list(nv_db.glob(f"{pkg_name}-*")):
        if (d / "desc").is_file() and f"%NAME%\n{pkg_name}\n" in (d / "desc").read_text():
            shutil.rmtree(d)

write_exec(
    nv_bin / "pacman",
    f"""#!/usr/bin/env python3
import os, pathlib, shutil, sys
log = pathlib.Path({str(nv_log)!r})
db = pathlib.Path({str(nv_db)!r})
vers = {nv_ver_map!r}
args = sys.argv[1:]
with log.open("a", encoding="utf-8") as f:
    f.write("pacman " + " ".join(args) + "\\n")
if "-Sp" in args:
    pkg = args[-1]
    sys.exit(0 if pkg in vers else 1)
if "-Si" in args:
    pkg = args[-1]
    if pkg == "nvidia-580xx-utils":
        print("Name            : nvidia-580xx-utils")
        print("Depends On      : opencl-nvidia-580xx  lib32-nvidia-580xx-utils")
        sys.exit(0)
    print(f"Name            : {{pkg}}")
    print("Depends On      : dkms")
    sys.exit(0)
if "-S" in args:
    pkgs = [a for a in args if not a.startswith("-") and a != "4"]
    if any(p.startswith("nvidia-580xx") for p in pkgs):
        for old in ("nvidia-open-dkms", "nvidia-utils"):
            for d in list(db.glob(f"{{old}}-*")):
                shutil.rmtree(d)
    if any(p in ("nvidia-open-dkms", "nvidia-utils") for p in pkgs):
        for old in ("nvidia-580xx-dkms", "nvidia-580xx-utils"):
            for d in list(db.glob(f"{{old}}-*")):
                shutil.rmtree(d)
    for p in pkgs:
        v = vers.get(p, "1.0-1")
        d = db / f"{{p}}-{{v}}"
        d.mkdir(parents=True, exist_ok=True)
        (d / "desc").write_text(f"%NAME%\\n{{p}}\\n\\n%VERSION%\\n{{v}}\\n", encoding="utf-8")
    sys.exit(0)
if "-Rns" in args:
    pkgs = [a for a in args if not a.startswith("-")]
    for p in pkgs:
        for d in list(db.glob(f"{{p}}-*")):
            shutil.rmtree(d)
    sys.exit(0)
sys.exit(0)
""",
)

write_exec(
    nv_bin / "locachy-pkg",
    f"""#!/usr/bin/env python3
import pathlib, shutil, sys
log = pathlib.Path({str(nv_log)!r})
db = pathlib.Path({str(nv_db)!r})
mf = pathlib.Path({str(nvroot / "etc/locachy/packages")!r})
vers = {nv_ver_map!r}
args = sys.argv[1:]
with log.open("a", encoding="utf-8") as f:
    f.write("locachy-pkg " + " ".join(args) + "\\n")
def mf_add(pkgs):
    lines = mf.read_text(encoding="utf-8").splitlines()
    for p in pkgs:
        if p not in lines:
            lines.append(p)
    mf.write_text("\\n".join(lines) + "\\n", encoding="utf-8")
def mf_rm(pkgs):
    s = set(pkgs)
    lines = [l for l in mf.read_text(encoding="utf-8").splitlines() if l.strip() not in s]
    mf.write_text("\\n".join(lines) + "\\n", encoding="utf-8")
if args[:3] == ["kernel", "headers", "on"]:
    v = vers["linux-cachyos-bore-headers"]
    d = db / f"linux-cachyos-bore-headers-{{v}}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "desc").write_text(f"%NAME%\\nlinux-cachyos-bore-headers\\n\\n%VERSION%\\n{{v}}\\n", encoding="utf-8")
    sys.exit(0)
if args and args[0] == "install":
    for p in args[1:]:
        v = vers.get(p, "1.0-1")
        d = db / f"{{p}}-{{v}}"
        d.mkdir(parents=True, exist_ok=True)
        (d / "desc").write_text(f"%NAME%\\n{{p}}\\n\\n%VERSION%\\n{{v}}\\n", encoding="utf-8")
    mf_add(args[1:])
    sys.exit(0)
if args and args[0] == "remove":
    for p in args[1:]:
        for d in list(db.glob(f"{{p}}-*")):
            shutil.rmtree(d)
    mf_rm(args[1:])
    sys.exit(0)
sys.exit(0)
""",
)

write_exec(
    nv_bin / "locachy-boot",
    f"""#!/bin/sh
printf 'locachy-boot %s\\n' "$*" >>"{nv_log}"
""",
)

nvenv = dict(os.environ)
nvenv.update({
    "PATH": f"{nv_bin}:{os.environ.get('PATH', '')}",
    "LOCACHY_NVIDIA_ROOT": str(nvroot),
    "LOCACHY_NVIDIA_SYS_PCI": str(sys_pci),
    "LOCACHY_NVIDIA_SYS_MODULE": str(out / "no-nv-mod"),
    "LOCACHY_NVIDIA_PKG": str(nv_bin / "locachy-pkg"),
    "LOCACHY_NVIDIA_BOOT": str(nv_bin / "locachy-boot"),
    "LOCACHY_NVIDIA_POWER": str(out / "no-nv-power"),
    "LOCACHY_NVIDIA_NO_NETNS": "1",
})

def run_nv(*args, expect_ok=True):
    r = subprocess.run([sys.executable, "-B", str(src / "link/locachy-nvidia"), *args],
                       env=nvenv, capture_output=True, text=True)
    if expect_ok:
        assert r.returncode == 0, f"locachy-nvidia {args} failed: {r.stderr}\\n{r.stdout}"
    else:
        assert r.returncode != 0, f"locachy-nvidia {args} unexpectedly succeeded"
    return r

nst0 = json.loads(run_nv("status", "--json").stdout)
assert nst0["branches"] == ["open", "580xx", "off"]
assert nst0["installed_branch"] == "off" and nst0["configured_branch"] == "off"
assert len(nst0["nvidia_gpus"]) == 1 and nst0["nvidia_gpus"][0]["device"] == "0x2460"

# Install open DKMS (new branch first)
run_nv("set", "open")
nst_open = json.loads(run_nv("status", "--json").stdout)
assert nst_open["installed_branch"] == "open" and nst_open["configured_branch"] == "open"
assert nst_open["dkms_version"] == "595.45.04-1" and nst_open["kernel_headers_installed"] is True
assert nst_open["gsp_disabled_configured"] is False
assert "gpu-unlock" in nst_open["gpu_unlock_advisory"]
mp_open = (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").read_text()
assert "blacklist nouveau" in mp_open and "options nvidia-drm modeset=1" in mp_open
assert "NVreg_EnableGpuFirmware=0" not in mp_open
nv_log_txt = nv_log.read_text()
assert "locachy-pkg kernel headers on" in nv_log_txt
assert "locachy-pkg install dkms" in nv_log_txt
assert "locachy-pkg install nvidia-open-dkms nvidia-utils" in nv_log_txt

# Multilib disabled guard refuses 580xx cleanly before touching installed open driver
(nvroot / "etc/locachy/rc.conf").write_text(
    (nvroot / "etc/locachy/rc.conf").read_text() + 'repo_multilib="0"\n', encoding="utf-8"
)
r_no_ml = run_nv("set", "580xx", expect_ok=False)
assert "multilib" in r_no_ml.stderr.lower()
assert json.loads(run_nv("status", "--json").stdout)["installed_branch"] == "open"

# Re-enable multilib and switch open -> 580xx atomically (--ask 4) with auto GSP off (gpu_unlock active)
(nvroot / "etc/locachy/rc.conf").write_text(
    (nvroot / "etc/locachy/rc.conf").read_text().replace('repo_multilib="0"\n', ""), encoding="utf-8"
)
run_nv("set", "580xx")
nst_580 = json.loads(run_nv("status", "--json").stdout)
assert nst_580["installed_branch"] == "580xx" and nst_580["dkms_version"] == "580.126.25-1"
assert nst_580["gsp_disabled_configured"] is True
assert nst_580["gpu_unlock_advisory"] == ""
mp_580 = (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").read_text()
assert "options nvidia NVreg_EnableGpuFirmware=0" in mp_580
mf_580 = (nvroot / "etc/locachy/packages").read_text()
assert "nvidia-580xx-dkms" in mf_580 and "nvidia-580xx-utils" in mf_580
assert "nvidia-open-dkms" not in mf_580 and "nvidia-utils\n" not in mf_580
assert "pacman -S --needed --noconfirm --ask 4 nvidia-580xx-dkms nvidia-580xx-utils" in nv_log.read_text()

# TSV status output matches settings.c parser contract
tsv_580 = run_nv("status", "--tsv").stdout
assert "installed\t580xx\n" in tsv_580
assert "dkms_version\t580.126.25-1\n" in tsv_580
assert "gsp_disabled\t1\n" in tsv_580

# GSP on/off/auto and sync-dropin when gpu_unlock_targets is disabled
run_nv("gsp", "on")
assert "options nvidia NVreg_EnableGpuFirmware=1" in (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").read_text()
run_nv("gsp", "auto")
assert "options nvidia NVreg_EnableGpuFirmware=0" in (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").read_text()
(nvroot / "etc/locachy/rc.conf").write_text(
    (nvroot / "etc/locachy/rc.conf").read_text().replace('gpu_unlock_targets="all"', 'gpu_unlock_targets="off"'),
    encoding="utf-8",
)
run_nv("sync-dropin")
assert "NVreg_EnableGpuFirmware" not in (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").read_text()

# Remove driver cleanly
run_nv("set", "off")
nst_off = json.loads(run_nv("status", "--json").stdout)
assert nst_off["installed_branch"] == "off" and nst_off["configured_branch"] == "off"
assert not (nvroot / "etc/modprobe.d/40-locachy-nvidia.conf").exists()
assert "nvidia-580xx-dkms" not in (nvroot / "etc/locachy/packages").read_text()
print("PASS locachy-nvidia open/580xx/off DKMS flow, atomic switch, multilib guard, GSP & sync-dropin")
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
