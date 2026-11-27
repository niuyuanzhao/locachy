# NVIDIA GPU Unlock and Machine GPU Power Profiles

LOCachy integrates discrete NVIDIA GPU configuration through the same native layers used for the rest of the system: machine profiles in `/usr/local/share/locachy/machines/`, `rc.conf` layering, `/etc/modules-load.d/` and `/etc/modprobe.d/` (loaded by `src/base/modload.c`), `locachy-boot` unified kernel images, and `locachy-jobs` system jobs driven from Settings.

## Graphics routing and GPU enablement

Under **Settings → Input & Output → NVIDIA GPU**, three controls do different jobs:

- **Hybrid graphics policy** follows `gpu_graphics_policy="auto"` (the default) or `"off"`. When the installed system can positively identify a built-in NVIDIA hybrid with the internal panel on the integrated GPU, it prefers that panel for the native display and puts the integrated DRM card first for labwc while retaining both cards' outputs. Unclear topology, an explicit Display GPU, or opting out preserves the existing selection. This does not install drivers, force an application onto NVIDIA, change firmware, or affect CUDA. It applies the next time graphics starts.
- **Firmware display routing** changes the physical MUX between hybrid and direct NVIDIA display on machines exposing the supported Linux `asus-armoury` firmware control. It shows the configured mode, not the active mode when a restart is pending. The system does not restart automatically.
- **NVIDIA discrete GPU** enables or disables the hardware via a separate `asus-armoury` control. Disabling it removes its outputs and CUDA until it is enabled again. It is refused unless hybrid routing is confirmed, no restart is pending and an integrated GPU has a connected internal panel; the driver may refuse a GPU that is in use. Ordinary driver-managed idle power and `gpu-unlock`'s RTD3 policy are unchanged.

Confirm firmware changes with a second Enter. `locachy-gpu-graphics status --json` reads back the configured values. `set-mux hybrid|discrete` and `set-dgpu on|off` are privileged system jobs. Unsupported, unreadable or conflicting firmware controls are not guessed. If the machine exposes a different switch only in BIOS Setup, use BIOS Setup; an older UKI does not undo an already written firmware setting.

## 0. NVIDIA DKMS Driver Management (`locachy-nvidia`)

LOCachy manages NVIDIA drivers strictly through **DKMS** (never prebuilt kernel-flavor module packages), ordered `auto` → `open` → `580xx` → `off` to match Arch Wiki guidance and LOCachy's multi-kernel selector (`locachy-pkg kernel select`):

- **`auto`**: reads PCI sysfs (`/sys/bus/pci/devices/*/vendor`, `device`, `class`) and resolves NVIDIA (`10de`) device descriptions from `/usr/share/hwdata/pci.ids` (falling back to `lspci -nn -d 10de:`), classifies each GPU against the Arch Wiki NVIDIA family table, installs the recommended branch's DKMS + utils packages (if the recommended branch is missing from the enabled repositories, the next supported branch is used and the fallback is printed), and records `nvidia_driver="auto"` in `rc.conf`.
- **`open`** (new, Arch Wiki default for Turing RTX 20+ / Ampere / Ada / Blackwell+): `nvidia-open-dkms` + `nvidia-utils` + `dkms` + active kernel headers. Open kernel modules always run with GSP-RM firmware.
- **`580xx`** (legacy / GSP-off fallback: Maxwell / Pascal / Volta, or a manual fallback for Turing / Ampere / Ada laptops where GSP-RM fails, with GSP firmware off via `NVreg_EnableGpuFirmware=0`): `nvidia-580xx-dkms` + `nvidia-580xx-utils` + `dkms` + active kernel headers. Note that `cachyos/nvidia-580xx-utils` depends on `lib32-nvidia-580xx-utils` from `[multilib]`; `locachy-nvidia` verifies `[multilib]` is enabled before modifying an installed driver.
- **`off`**: removes installed NVIDIA driver packages, clears manifest leaves, removes `/etc/modprobe.d/40-locachy-nvidia.conf`, and rebuilds the UKI.

### Automatic PCI family-to-branch mapping (Arch Wiki table)

| GPU family (codename in `pci.ids`) | Supported branches | `auto` recommendation | Notes |
| --- | --- | --- | --- |
| Blackwell / Hopper (`GB*`, `GH*`) and newer | `open` | `open` | `580xx` does not support Blackwell; manual `set 580xx` is refused unless `--force` is passed. |
| Ada Lovelace (`AD*`), Ampere (`GA*`), Turing (`TU*`) | `open`, `580xx` | `open` (desktop and laptop; still `open` when `gpu_unlock_targets` is active) | Ampere laptop GPUs (`GA*M`, mobile/laptop name, or laptop DMI chassis) get the Arch Wiki note: on some Ampere laptops GSP-RM causes complete failure; if this machine is affected, switch manually to `580xx` with GSP off. |
| Volta (`GV*`), Pascal (`GP*`), Maxwell (`GM*`) | `580xx` | `580xx` | Pre-Turing GPUs lack GSP; manual `set open` is refused unless `--force` is passed. |
| Kepler (`GK*`), Fermi (`GF*`), Tesla (`G8*`/`G9*`/`GT2*`), Curie (`G7*`/`NV4*`) | none | `off` (refused by `set auto`) | Unsupported by `open` and `580xx`; use `nouveau` or AUR `470xx`/`390xx`. |
| Unknown NVIDIA PCI ID (unlisted future card) | `open`, `580xx` | `open` | Treated as a newer card until `pci.ids` adds its entry. |

When multiple NVIDIA GPUs are present, `locachy-nvidia` intersects their supported branches: if every GPU supports `open` and `580xx`, the single-GPU rules apply; if only one branch supports all GPUs, that branch is recommended; if one requires `open` while another requires `580xx` (or is legacy/unsupported), `status` reports the conflict and `set auto` refuses cleanly.

Settings exposes two rows at the top of **Input & Output → NVIDIA GPU**:

- **NVIDIA driver (DKMS)** (`nvidia_driver="auto"|"open"|"580xx"|"off"`): picker row (`Enter` → `Left`/`Right` → `Enter` applies via `gpu-driver-set`), showing detected GPU family and recommended branch from `locachy-nvidia status --tsv`.
- **NVIDIA GSP firmware** (`nvidia_gsp="auto"|"off"|"on"`): applies to `580xx` via `gpu-driver-gsp`. `auto` writes `options nvidia NVreg_EnableGpuFirmware=0` whenever `gpu_unlock_targets` is active; `off` forces it off; `on` forces GSP on. `open` requires GSP-RM, so `locachy-nvidia` leaves GSP enabled on `open`; when `gpu-unlock` is also active, `status` shows an informational note (GSP-RM does not block `gpu-unlock`, and branch 615+ restored probe-time init after the 595 `NVreg_GpuInitOnProbe` regression, per `Zagitta/gpu-unlock` #135 — on an affected 595 build or on unstable GSP hardware, set `NVreg_GpuInitOnProbe=1` or switch manually to `580xx`).

CLI interface:

```sh
locachy-nvidia status [--json | --tsv]
locachy-nvidia set auto|open|580xx|off [--force]
locachy-nvidia gsp auto|off|on
locachy-nvidia sync-dropin
```

Manual overrides (`set open` on Maxwell/Pascal/Volta or `set 580xx` on Blackwell) are refused before touching packages unless `--force` is given. Switching between `open` and `580xx` replaces conflicting packages atomically (`pacman -S --needed --noconfirm --ask 4`), updates `/etc/locachy/packages` (and `locachy-pkg`'s Vulkan detection recognizes `nvidia-580xx-utils` as well as `nvidia-utils`), writes `/etc/modprobe.d/40-locachy-nvidia.conf` (`blacklist nouveau`, `options nouveau modeset=0`, `options nvidia-drm modeset=1`, `softdep nvidia post: nvidia-uvm`, and optional `NVreg_EnableGpuFirmware`), refreshes `nvidia-powerd` when enabled, and rebuilds the UKI. `locachy-gpu-unlock` calls `locachy-nvidia sync-dropin` automatically whenever `gpu-unlock` is installed, toggled, or removed.

## 1. Pre-driver NVIDIA GPU Unlock (`locachy-gpu-unlock`)

Some NVIDIA GPUs carry firmware speed selectors that are applied before the driver initializes. `locachy-gpu-unlock` integrates upstream [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) directly into LOCachy. Upstream's kernel module lifts that limit for the PCI IDs in its recipe table; its README states the exact effect and scope. LOCachy adds one action in Settings (`Input & Output -> NVIDIA GPU -> NVIDIA GPU unlock (gpu-unlock)` -> `Enter`) or the CLI:

```sh
locachy-gpu-unlock status [--json]
locachy-gpu-unlock install [--target all|0000:01:00.0] [--repo URL] [--branch NAME]
locachy-gpu-unlock set off|all|0000:01:00.0
locachy-gpu-unlock remove
```

How it works:

1. **Upstream tracking and minimal PCI ID overlay**: Upstream source metadata lives in `/usr/local/share/locachy/gpu-unlock/upstream.json`. On `install`, `locachy-gpu-unlock` clones the upstream repository through `locachy-net` (when active), idempotently inserts any missing PCI IDs listed in `extra_pci_ids` (such as `GA103(0x2460)` for RTX 3080 Ti Laptop) into `kernel/gpu_unlock/recipes.c`, and runs `makepkg` as an unprivileged build user (`locachy-builder` / `builder` / `nobody`) via `setpriv`. Upstream's own `PKGBUILD` downloads the pinned NVIDIA firmware inputs and verifies their SHA-256 checksums.
2. **Package manifest and DKMS**: `locachy-gpu-unlock` ensures kernel headers and build dependencies (`dkms`, `git`, `python`, `zstd`, `openssl`) are installed via `locachy-pkg`, installs `gpu-unlock-dkms` and `gpu-unlock-firmware`, and records both names in `/etc/locachy/packages`.
3. **Native boot ordering without modifying initramfs**: LOCachy does not bundle `nvidia` into the initramfs. After `switch_root`, process 1 calls `modules_start()` (`src/base/modload.c`), which loads `/etc/modules-load.d/*.conf` strictly before PCI `coldplug()` loads `nvidia`, and also honors `softdep` rules in `/etc/modprobe.d/`. `locachy-gpu-unlock` writes:
   - `/etc/modules-load.d/50-locachy-gpu-unlock.conf` (`gpu_unlock`)
   - `/etc/modprobe.d/50-locachy-gpu-unlock.conf` (`softdep nvidia pre: gpu_unlock` and `options nvidia NVreg_DynamicPowerManagement=0x00`, keeping the dGPU powered so RTD3 power-off does not reset unlocked speed selectors)
   - `gpu_unlock_targets="..."` in `/etc/locachy/rc.conf`
4. **UKI kernel command line**: `locachy-boot` reads layered `gpu_unlock_targets` (`all` or comma-separated `DDDD:BB:DD.F` PCI BDFs) and appends `gpu_unlock.targets=...` to the unified kernel image command line, then rebuilds the boot image.
5. **Removal**: Pressing `Delete` twice on the Settings row (or running `locachy-gpu-unlock remove`) removes the drop-in files, clears `gpu_unlock_targets`, removes the packages and manifest entries, and rebuilds the UKI.

Power-state note: upstream `gpu-unlock` hooks S3 suspend/resume to re-apply speed selectors after sleep; it does not currently re-apply across S4 hibernation.

Driver compatibility note: upstream reports (`Zagitta/gpu-unlock` #135) show that GSP-RM itself does not prevent the unlock. The breakage on branch 595 came from NVIDIA's `NVreg_GpuInitOnProbe` default of `0` (lazy init re-latched the speed selector); branch 615+ returns the default to probe-time `AUTO`. The HP RTX 3080 Ti Laptop (`GA103`, `0x2460`) is reported working (`Zagitta/gpu-unlock` #88). One reported case (#206) had selectors reverted under the 580-series open module while the proprietary 580xx with GSP off kept them latched, which is why `580xx` stays available as a manual fallback.

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
