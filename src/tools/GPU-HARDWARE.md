# NVIDIA GPU Unlock and Machine GPU Power Profiles

LOCachy integrates discrete NVIDIA GPU configuration through the same native layers used for the rest of the system: machine profiles in `/usr/local/share/locachy/machines/`, `rc.conf` layering, `/etc/modules-load.d/` and `/etc/modprobe.d/` (loaded by `src/base/modload.c`), `locachy-boot` unified kernel images, and `locachy-jobs` system jobs driven from Settings.

## 1. Pre-driver NVIDIA GPU Unlock (`locachy-gpu-unlock`)

Some NVIDIA GPUs carry firmware speed selectors that are applied before the driver initializes. `locachy-gpu-unlock` integrates upstream [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) directly into LOCachy. Upstream's kernel module lifts that limit for the PCI IDs in its recipe table; its README states the exact effect and scope. LOCachy adds one action in Settings (`Input & Output -> NVIDIA GPU -> NVIDIA GPU unlock (gpu-unlock)` -> `Enter`) or the CLI:

```sh
locachy-gpu-unlock status [--json]
locachy-gpu-unlock install [--target all|0000:01:00.0] [--repo URL] [--branch NAME]
locachy-gpu-unlock set off|all|0000:01:00.0
locachy-gpu-unlock remove
```

How it works:

1. **Upstream tracking and minimal PCI ID overlay**: Upstream source metadata lives in `/usr/local/share/locachy/gpu-unlock/upstream.json`. On `install`, `locachy-gpu-unlock` clones the upstream repository through `locachy-net` (when active), idempotently inserts any missing PCI IDs listed in `extra_pci_ids` (such as `GA103(0x2460)` for RTX 3080 Ti Laptop) into `kernel/gpu_unlock/recipes.c`, and runs `makepkg` as an unprivileged build user (`locachy-build` / `builder` / `nobody`). Upstream's own `PKGBUILD` downloads the pinned NVIDIA firmware inputs and verifies their SHA-256 checksums.
2. **Package manifest and DKMS**: `locachy-gpu-unlock` ensures kernel headers and build dependencies (`dkms`, `git`, `python`, `zstd`, `openssl`) are installed via `locachy-pkg`, installs `gpu-unlock-dkms` and `gpu-unlock-firmware`, and records both names in `/etc/locachy/packages`.
3. **Native boot ordering without modifying initramfs**: LOCachy does not bundle `nvidia` into the initramfs. After `switch_root`, process 1 calls `modules_start()` (`src/base/modload.c`), which loads `/etc/modules-load.d/*.conf` strictly before PCI `coldplug()` loads `nvidia`, and also honors `softdep` rules in `/etc/modprobe.d/`. `locachy-gpu-unlock` writes:
   - `/etc/modules-load.d/50-locachy-gpu-unlock.conf` (`gpu_unlock`)
   - `/etc/modprobe.d/50-locachy-gpu-unlock.conf` (`softdep nvidia pre: gpu_unlock` and `options nvidia NVreg_DynamicPowerManagement=0x00`, keeping the dGPU powered so RTD3 power-off does not reset unlocked speed selectors)
   - `gpu_unlock_targets="..."` in `/etc/locachy/rc.conf`
4. **UKI kernel command line**: `locachy-boot` reads layered `gpu_unlock_targets` (`all` or comma-separated `DDDD:BB:DD.F` PCI BDFs) and appends `gpu_unlock.targets=...` to the unified kernel image command line, then rebuilds the boot image.
5. **Removal**: Pressing `Delete` twice on the Settings row (or running `locachy-gpu-unlock remove`) removes the drop-in files, clears `gpu_unlock_targets`, removes the packages and manifest entries, and rebuilds the UKI.

Power-state note: upstream `gpu-unlock` hooks S3 suspend/resume to re-apply speed selectors after sleep; it does not currently re-apply across S4 hibernation.

## 2. Machine GPU Power Profiles (`locachy-gpu-power`)

Machine-specific discrete GPU power controls are declared in machine profiles under `src/machines/` (installed to `/usr/local/share/locachy/machines/`), alongside display card defaults and `boot_cmdline`.

### HP OMEN 17-ck1xxx (`hp-omen-17-ck1.conf`)

Board `8A1A` (Intel i9-12900HX + NVIDIA RTX 3080 Ti Laptop):

- `display_card="xe"`, `extdisplay_card="xe"` — keeps the direct framebuffer and extended display on the Intel iGPU without waking the dGPU.
- `boot_cmdline="nouveau.modeset=0 nvidia_drm.modeset=1 i915.force_probe=!4688 xe.force_probe=4688"` — hands PCI ID `0x4688` to the `xe` driver (`i915` is excluded for it) and keeps `nouveau` disabled. (`locachy-boot` layers `boot_cmdline` from the active machine profile unless overridden in `/etc/locachy/rc.conf`, and permits `!` for `force_probe` exclusions.)
- `gpu_power_method="hp-omen-wmi"`, `gpu_power_mode="full"`, `nvidia_powerd="1"` — unlocks the BIOS WMI GPU thermal/power modes via the in-tree DKMS module `src/machines/hp-omen-gpu/` (`hp_omen_gpu`), raising the RTX 3080 Ti Laptop from the 115 W factory cap to 150 W cTGP and up to 175 W with Dynamic Boost (`nvidia-powerd`).

`hp_omen_gpu` calls the same HP BIOS WMI query as upstream `hp-wmi` (`GUID 5FB7F034-2C63-45E9-BE91-3D44E2C707E4`, `HPWMI_GM 0x20008`, query `0x21` read / `0x22` write), always reads back and preserves `gpu_slowdown_temp` verbatim, exports no `MODULE_ALIAS`, and leaves no polling daemon or background timer running.

### Modes and Settings Controls

Settings exposes two rows under `Power & Battery -> NVIDIA GPU power`:

- **GPU power mode** (`gpu_power_mode`):
  - `auto` — follows LOCachy's `power_profile` (`performance` -> `full`, `balanced` -> `ctgp`, `power` -> `eco`, `auto` -> `ctgp` on AC / `stock` on battery)
  - `full` — `ctgp=1 ppab=1 dstate=1` (150 W cTGP + Dynamic Boost to 175 W, D1 100%)
  - `ctgp` — `ctgp=1 ppab=0 dstate=1` (150 W cTGP fixed, D1 100%)
  - `stock` — `ctgp=0 ppab=0 dstate=1` (factory 115 W TGP, D1 100%)
  - `eco` — `ctgp=0 ppab=0 dstate=2` (factory TGP, D2 50%)
- **NVIDIA Dynamic Boost (nvidia-powerd)** (`nvidia_powerd`): toggles `service_nvidia_powerd="/usr/bin/nvidia-powerd"` under process 1 when `/usr/bin/nvidia-powerd` is installed.

CLI interface:

```sh
locachy-gpu-power status [--json]
locachy-gpu-power apply [auto|full|ctgp|stock|eco]
locachy-gpu-power nvidia-powerd on|off
locachy-gpu-power remove
```

Selecting `hp-omen-17-ck1` in Settings (`System -> About This System -> Machine profile`) automatically applies its GPU power profile and rebuilds the boot image with its `boot_cmdline`.
