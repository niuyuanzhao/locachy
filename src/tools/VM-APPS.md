# Managed VM Apps (`vm="1"`)

[中文](VM-APPS.zh.md)

A VM app is a KVM virtual machine declared in `/etc/locachy/containers/NAME.conf` with `vm="1"`. Home lists it alongside containers, and `locachy-app` handles its `start`, `resume`, `freeze`, `stop`, and `remove` lifecycle in the shared `labwc` Wayland compositor inside its own `/sys/fs/cgroup/app_NAME` cgroup.

VM apps have no OverlayFS layer and no container package build. `locachy-app build`, `install`, and `rollback` refuse them, `locachy-pkg` skips them when planning container updates, and `packages` does not list them.

## Configuration keys

All `vm_*` keys come from a closed set (`VM_KEYS` in `link/locachy-app`). Any unknown `vm_*` key is refused at launch, keys that do not apply to the chosen `vm_kind` are refused, and no key passes raw QEMU arguments.

| Key | Values | Meaning |
| --- | --- | --- |
| `vm` | `"1"` | Marks the entry as a managed VM app. |
| `vm_kind` | `winmaint`, `compute`, `custom` | Backend selector. Empty defaults to `custom`. |
| `vm_name` | name | Target VM name for `vm_kind="compute"` (defaults to `NAME`; refused for other kinds). |
| `vm_freeze` | `qmp`, `none` | Freeze behaviour (default `qmp`). `none` turns `freeze` into a no-op. |
| `vm_display` | `gtk`, `sdl`, `none` | QEMU display backend (default `gtk`). |
| `vm_memory` | integer MiB | Guest RAM in MiB (`custom` default `4096`; forwarded to `winmaint`; refused for `compute`). |
| `vm_cpus` | integer | vCPU count (`custom` default `4`; forwarded to `winmaint`; refused for `compute`). |
| `vm_iso` | absolute path | Installation ISO (`cd0`), a regular file (not a directory or device); forwarded to `winmaint`; refused for `compute`. It boots first (`bootindex=1`), except a Windows ISO that `locachy-winiso` supplies, which boots first only while the disk is blank. Unset on a Windows guest: `locachy-winiso path windows` supplies its verified ISO when there is one ([WINISO.md](WINISO.md)). |
| `vm_base` | absolute path | Read-only base disk (`raw` or `qcow2`) for `custom`; a copy-on-write overlay is created on first start. |
| `vm_disk` | absolute path | Read-write disk (`raw` or `qcow2`) used directly by `custom`. |
| `vm_uefi` | `"0"`, `"1"` | OVMF firmware (`custom` only; `"1"` default for `vm_os="windows"`, `"0"` for `linux`). |
| `vm_tpm` | `"0"`, `"1"` | Emulated TPM 2.0 via `swtpm` (`custom` only; defaults to `"1"` for `windows` when `swtpm` is installed, else `"0"`). |
| `vm_share_home` | `"0"`, `"1"` | Share uid 1000's entire home directory via `virtiofs` as mount tag `home` (`custom` only; default `"0"`). |
| `vm_virtio_iso` | absolute path | Second read-only CD-ROM (`cd1`), e.g. `virtio-win.iso` (`custom`), a regular file; for `winmaint`, the virtio-win drivers it stages (`--virtio-iso`). A Windows guest without it gets the package's ISO from `locachy-winiso path virtio-win` when that is installed. With virtio-win, the drivers (`viostor`, `NetKVM`) are also staged into `drivers.img` as `$WinPEDriver$` and attached as `usb-storage,drive=drv0,bus=xhci.0,removable=on`; marking the seed removable makes Windows Setup search it for `$WinPEDriver$` and load the drivers automatically. |
| `vm_os` | `linux`, `windows` | Guest OS profile (`custom` only; default `linux`); selects CPU flags and device defaults. |
| `vm_nic` | `virtio`, `e1000e` | NIC model when `network="1"` (`custom`: default `virtio` for `linux`; for `windows`, `virtio` on a blank disk that Setup installs with the driver seed, otherwise `e1000e`; `winmaint`: default `e1000e`). A Windows default that is chosen is written into the configuration. |
| `vm_gpu` | `2d`, `gl` | Display adapter (`custom` only): `2d` (`virtio-vga`, `gl=off`, default) or `gl` (`virtio-vga-gl`, `gl=on`, Linux guests only). |
| `vm_disk_bus` | `virtio`, `nvme`, `sata` | Disk bus (`custom`: all three; `winmaint`: `virtio` or `nvme`). Default `virtio`. For `windows`: `virtio` only on a blank disk that Setup installs with the driver seed; otherwise `nvme`, the bus Windows boots without drivers (a disk with data may hold a Windows that has no `viostor`). A Windows default that is chosen is written into the configuration. |
| `vm_audio` | `"0"`, `"1"` | Attach `ich9-intel-hda` + `hda-duplex` over PulseAudio (`custom` only; default `"0"`); refused unless `/run/pulse/restricted/native` exists (the sound server must be running). |
| `network` | `"0"`, `"1"` | `"1"` attaches the VM to `locachy-net` (and `-nic user,model=<nic>` in `custom`); unset or `"0"` gives a loopback-only namespace and `-nic none`. |

### Kind-specific key checks

- For `vm_kind="winmaint"`, only `vm_freeze`, `vm_display`, `vm_memory`, `vm_cpus`, `vm_iso`, `vm_nic`, `vm_disk_bus`, and `vm_virtio_iso` are accepted (forwarded as `--memory`, `--cpus`, `--iso`, `--nic`, `--disk-bus`, `--virtio-iso`); `vm_name` and the other `custom`-only keys (`vm_base`, `vm_disk`, `vm_uefi`, `vm_tpm`, `vm_share_home`, `vm_os`, `vm_gpu`, `vm_audio`) are refused.
- For `vm_kind="compute"`, only `vm_name`, `vm_freeze`, and `vm_display` are accepted; `vm_memory`, `vm_cpus`, `vm_iso`, and all `custom`-only keys are refused (a compute VM keeps its own hardware definition).
- For `vm_kind="custom"` (or omitted `vm_kind`), `vm_name` is refused.

### Network policy

`network="1"` enters `/run/netns/locachy-net` via `nsenter --net` so the VM inherits LOCachy's network stack (including Tor, Clash, and Tailscale rules). If `/run/netns/locachy-net` does not exist, `locachy-app` fails closed into a private loopback-only network namespace rather than exposing the host network. On a `custom` VM, `network="1"` always passes `-nic user,model=<nic>`; unset or `"0"` passes `-nic none` inside the loopback-only namespace.

### Declarative `custom` VMs vs `command=`

For `vm_kind="custom"` (or omitted `vm_kind`):

- Without `command=`, the configuration must set **exactly one** of `vm_base` or `vm_disk`. Setting both, or neither, is refused.
- Setting `command=` (with optional `arguments=`) is the single escape hatch for an explicit launcher program (not raw QEMU arguments). Combining `command=` with `vm_base` or `vm_disk` is refused.
- Every path given in `vm_base`, `vm_disk`, `vm_iso`, and `vm_virtio_iso` must be absolute, must not contain `.` or `..` path components, must not lie under `/var/lib/locachy/vms` (the managed state directory deleted by `remove`), and must already exist. `vm_iso` and `vm_virtio_iso` must be regular files (not directories or devices), also when they are forwarded to `winmaint`.
- LOCachy never runs `chmod` or `chown` on user-supplied images. `vm_base`, `vm_iso`, and `vm_virtio_iso` must be readable by uid 1000; `vm_disk` must be both readable and writable by uid 1000.
- KVM is auto-detected from `/dev/kvm` (readable and writable, otherwise TCG fallback), and the disk I/O engine is `aio=io_uring`.

## Kinds

- **`winmaint`**: runs `locachy-winmaint vm --display <display> --qmp-sock <sock>` (plus optional `--memory`, `--cpus`, `--iso`, `--nic`, `--disk-bus`, `--virtio-iso`). Keeps the bare-metal dual-boot `stornvme.sys`-compatible NVMe disk wrapper and four-layer UEFI protection described in [WINMAINT.md](WINMAINT.md).
- **`compute`**: runs `locachy-compute vm run <vm_name> --display <display> --qmp-sock <sock>` in the foreground inside `app_NAME` (see [COMPUTE.md](COMPUTE.md)).
- **`custom`**: either builds a QEMU/KVM command declaratively from `vm_base`/`vm_disk` and the `vm_*` keys, or runs `command=`/`arguments=`.

## Remote NVIDIA experiment management

A `vm_kind="compute"` VM is an optional kernel-isolation backend, not a requirement for ordinary NVIDIA experiments. The primary management path remains the normal LOCachy user over **OpenSSH through Tailscale**, using `ssh` for `locachy-compute` and `scp`/`sftp` for inputs and results. The optional HTTPS/mTLS API is not required for this path; Thunderbolt is a separate optional heterogeneous path (see [COMPUTE.md](COMPUTE.md)).

The SSH server must be reachable only on the node's tailnet addresses and must use public-key authentication for the uid-1000 user; do not use root login, password SSH, or Tailscale SSH as a substitute. The SSH user key, SSH host key, tailnet identity, optional compute mTLS credentials, and any display PSK are separate identities and must be trusted or revoked separately. Existing password-authenticated `sudo` remains the privilege boundary; an SSH key does not grant passwordless sudo. Use the established host-key fingerprint before adding the server to `known_hosts`.

On first start, `service_tailnet_ssh` generates and persists its Ed25519 host key on the node if one does not already exist. The administrator enrolls the operator’s public key in `/etc/locachy/tailnet-ssh/authorized_keys/<user>` as described in [COMPUTE.md](COMPUTE.md).

Keep model weights, datasets, checkpoints, logs, and results on a mounted `compute` or `shared` storage volume, outside managed VM state. Bind only the required paths to a compute environment or VM, and verify the mount with `findmnt` before copying files. `scp`/`sftp` do not verify the backing disk. A detached native-container job must run in its own compute cgroup, survive SSH logout, retain stdout/stderr and a terminal exit code, and leave no workload process after cancellation. Treat a record with no exit code as unverified; see [COMPUTE.md](COMPUTE.md) for the complete flow. A VM itself remains under `locachy-app` lifecycle control and is not a detached SSH job.

## Privilege model

- `compute` and `custom` VMs never run as root when `locachy-app` is invoked as real host root. QEMU and its helpers (`qemu-img`, `swtpm`, `virtiofsd`) are launched through `setpriv` as uid 1000 with the `kvm` supplementary group (when present) and `no_new_privs`. Only the shared Wayland socket (`locachy-0`) and restricted sound directories are bind-mounted into a private tmpfs runtime directory owned by uid 1000, so QEMU never accesses root's `0700` `/run/user/0`.
- Only `vm_kind="winmaint"` retains root, because `locachy-winmaint vm` needs `dmsetup` and `losetup` to assemble its virtual `dm-linear` disk.
- In unprivileged test namespaces (where the caller is not real host root), `locachy-app` does not switch uid.

## State directory and removal

Per-VM managed state lives in `/var/lib/locachy/vms/NAME`:

- Parent `/var/lib/locachy/vms` is mode `0711`; `/var/lib/locachy/vms/NAME` is mode `0700` owned by uid 1000 (on real host root).
- On creation, `chattr +C` is applied best-effort so Btrfs disables copy-on-write (and therefore Btrfs checksumming and compression) for VM images stored there; on non-Btrfs filesystems the failure is ignored.
- Holds `disk.qcow2` (the CoW overlay created once by `qemu-img create -f qcow2 -b <vm_base>`, leaving `vm_base` read-only), `drivers.img` (for `vm_os="windows"` with virtio-win: the FAT driver seed containing `$WinPEDriver$`, attached read-only; made when missing and rebuilt when the virtio-win ISO is newer), `ovmf-vars.fd` (private writable OVMF variables), `swtpm/` (TPM state), and runtime sockets (`qmp.sock`, `swtpm.sock`, `vfs-home.sock`).

`locachy-app remove NAME` refuses while the VM is running. When stopped, it deletes `/etc/locachy/containers/NAME.conf`, `/run/locachy/apps/NAME*`, and `/var/lib/locachy/vms/NAME`. It never deletes user-supplied `vm_base` or `vm_disk` files, and never deletes Maintenance Windows partitions.

## Helpers and lifecycle (freeze, resume, stop)

Helper daemons (`swtpm` and `virtiofsd`) are spawned **after** the launcher enters `/sys/fs/cgroup/app_NAME`, so they share the cgroup with QEMU and are frozen and stopped together with it.

- **Freeze (`vm_freeze="qmp"`, default)**: syncs the clipboard if available, sends QMP `{"execute":"stop"}` to halt vCPUs at a clean boundary (so guest clocks and virtio queues stay consistent), writes `/run/locachy/apps/NAME/vm-paused`, and then freezes the cgroup through the compositor-gated clipboard handover (`clipboard freeze`). There is no unguarded `cgroup.freeze` fallback: if the clipboard handover is unavailable or refused, `locachy-app` sends QMP `cont`, clears `vm-paused`, and fails the freeze.
- **Freeze (`vm_freeze="none"`)**: returns 0 without pausing vCPUs or freezing the cgroup (useful during OS installs or Windows Update).
- **Resume**: cancels any memory reclaim, thaws `cgroup.freeze=0` and waits for `frozen 0`, then sends QMP `{"execute":"cont"}` if `vm-paused` was set.
- **Stop**: thaws the cgroup, sends QMP `cont` if paused so the guest can handle ACPI, sends QMP `{"execute":"system_powerdown"}` and waits up to ~8 seconds, then sends `SIGTERM` to the PID-1 reaper and waits up to ~3 seconds (letting `locachy-winmaint`'s `finally:` clean up `dm-linear`/`losetup`/`swtpm`), and falls back to `cgroup.kill` only if processes still remain.
- **Single-instance semantics**: `start`, `open`, `again`, and `new` on an already-running VM never launch a second QEMU process on the same disk; if frozen, they resume it.
- **Helper failure**: the EXIT handler is installed before the first helper starts. If a helper fails to start or times out, the helpers already started are stopped, the VM does not start, and no ready marker is made, so Home never shows a window for it.

## Sharing the home directory (`vm_share_home="1"`)

When `vm_share_home="1"`, `locachy-app` starts `virtiofsd` (`/usr/lib/virtiofsd`, `/usr/libexec/virtiofsd`, or `PATH`; `LOCACHY_VIRTIOFSD` in tests) with `--sandbox=namespace --cache=auto` sharing uid 1000's **entire home directory**, and configures QEMU with shared memfd RAM (`-object memory-backend-memfd,id=mem,size=...M,share=on`, `-machine ...,memory-backend=mem`) and `-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=home`.

Mounting inside the guest is not automatic:

- **Linux guest**:
  ```sh
  mount -t virtiofs home /mnt/home
  ```
- **Windows guest**: install the `viofs` driver from `virtio-win` (`vm_virtio_iso`), install WinFsp, and enable/start the `VirtioFsSvc` service inside Windows.

## Window model

In this initial scope, the QEMU GTK (or SDL) window **is** the application window, and the guest OS decides what is displayed inside it. There is no guest agent and no seamless per-program window forwarding. If you want a VM to open directly into one full-screen program, configure autologon and program autostart inside the guest image yourself.

## Window attribution

Home's status, and `labwc-control`'s show, close, focus and window list, find a VM's window by QEMU's window title and Wayland app id. Every QEMU window has the app id `qemu`, so the title carries the tag the launcher gave the machine with `-name`:

- `custom`: `locachy-vm[NAME]`. The GTK title is `QEMU (locachy-vm[NAME])`, with ` [Paused]` or `: label` appended when QEMU adds one; the SDL title is `QEMU (locachy-vm[NAME]-N)`.
- `compute`: `locachy-compute[VMNAME]` (`vm_name`), set by `locachy-compute`.
- `winmaint`: `locachy-winmaint`.

A VM's window matches only when its title carries that VM's tag and its app id starts with `qemu`. The brackets make the tags unambiguous: an app name cannot contain brackets, so `debian` never matches the window of `debian-12` (`locachy-vm[debian-12]`). A window whose title carries a VM tag never belongs to a plain container or to a `command=` VM through its app id; a `command=` VM is otherwise matched by its command's app id.

## Closing the window

Closing a VM's window (its own button, the bar, the overview) does not stop the VM. The display argument carries `window-close=off`, so QEMU keeps running when its window is closed. Stop the VM from Home, or shut the guest down from inside it.

## Windows guests

Setting `vm_os="windows"` enables:

- KVM CPU flags: `host,hv-relaxed,hv-vapic,hv-spinlocks=0x1fff,hv-time,hv-vpindex,hv-synic,hv-stimer,hv-stimer-direct,hv-reset,hv-frequencies,hv-tlbflush,hv-ipi` (the same set `winmaint` uses; Linux guests use plain `host`; TCG fallback uses `max`).
- Defaults: a blank disk that Setup installs with the driver seed (virtio-win available, from `vm_virtio_iso` or `locachy-winiso`) gets `vm_disk_bus="virtio"` (virtio-blk with an iothread and io_uring) and `vm_nic="virtio"`. Any other Windows disk gets `vm_disk_bus="nvme"` and `vm_nic="e1000e"`, which Windows boots and runs without extra drivers. The values chosen are written into the configuration. Also `vm_uefi="1"` (with `smm=on`), and `vm_tpm="1"` when `swtpm` is installed.
- Installation: `vm_iso`, or `locachy-winiso fetch windows` for Microsoft's ISO, verified against its published SHA-256 ([WINISO.md](WINISO.md)). The installer boots first only while the disk is blank; once Windows is on the disk it is attached without boot priority.

Notes for Windows installation:

- Attach `virtio-win.iso` with `vm_virtio_iso="/path/to/virtio-win.iso"` to install `viostor`, `NetKVM`, `viogpudo`, and `viofs` inside the guest.
- With `virtio-win` available (`vm_virtio_iso` or `locachy-winiso`), `locachy-app` attaches `drivers.img` (`usb-storage,drive=drv0,bus=xhci.0,removable=on`) so Windows Setup loads `viostor` and `NetKVM` automatically without manual driver loading. On a disk that already holds Windows without `viostor`, keep `vm_disk_bus="nvme"` or `vm_disk_bus="sata"` (both boot without extra storage drivers) and `vm_nic="e1000e"` until `viostor` and `NetKVM` are installed inside the guest, then switch to `vm_disk_bus="virtio"` / `vm_nic="virtio"`. If `virtio-win` is absent or the seed cannot be built, `vm_disk_bus="virtio"` requires loading `viostor` during Setup.
- `vm_gpu="gl"` (`virtio-vga-gl` / virgl) is refused for `vm_os="windows"`.
- For bare-metal dual-boot Maintenance Windows (`vm_kind="winmaint"`), see [WINMAINT.md](WINMAINT.md).

## Example configurations

Linux desktop VM from a read-only base image:

```conf
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="/home/user/VMs/debian-base.qcow2"
vm_memory="4096"
vm_cpus="4"
vm_uefi="1"
vm_share_home="1"
network="1"
```

Windows VM with `virtio-win` ISO:

```conf
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="/home/user/VMs/win11.qcow2"
vm_virtio_iso="/home/user/Downloads/virtio-win.iso"
vm_memory="8192"
vm_cpus="4"
vm_nic="e1000e"
network="1"
```

## Test seams

For integration tests, `locachy-app` checks a fixed list of root-only environment overrides (each must be an absolute path and requires uid 0). The fifteen overrides in that list are the eight general ones (`LOCACHY_CGROUP_BASE`, `LOCACHY_CONT_BASE`, `LOCACHY_CONT_CONF`, `LOCACHY_APP_RUNTIME`, `LOCACHY_APP_INIT`, `LOCACHY_CLIPBOARD_TOOL`, `LOCACHY_TOR_ENFORCE`, `LOCACHY_RC_CONF`) and these seven VM-specific ones:

- `LOCACHY_WINMAINT_TOOL` — overrides `/usr/local/bin/locachy-winmaint`
- `LOCACHY_COMPUTE_TOOL` — overrides `/usr/local/bin/locachy-compute`
- `LOCACHY_VM_STATE` — redirects `/var/lib/locachy/vms`
- `LOCACHY_QEMU_BIN` — overrides `qemu-system-x86_64`
- `LOCACHY_VIRTIOFSD` — overrides `virtiofsd`
- `LOCACHY_SWTPM` — overrides `swtpm`
- `LOCACHY_WINISO_TOOL` — overrides `/usr/local/bin/locachy-winiso`

## Tests and what needs real hardware

The automated tests live in `src/tools/test-app-vm.sh`. They run unprivileged on Linux in a user and mount namespace, with fixture cgroup files, a fake QMP server standing in for QEMU, and fake `swtpm`, `virtiofsd` and launcher helpers. Overlay creation uses real `qemu-img`; when `/dev/kvm` is usable, the suite also checks the argv with real `qemu-system-x86_64` and drives it over QMP. They cover: refusal of unknown and misplaced keys, path and ISO checks, the argv of Linux, Windows, SDL and GTK VMs, window tags and `window-close=off`, the freeze, resume and stop ordering, helper startup failure, the defaults of Windows guests with and without virtio-win (recorded in the configuration; explicit keys win), and ISOs supplied by `locachy-winiso`.

Only real hardware can confirm: the QEMU GTK window inside `labwc` (including what closing it does), real `virtiofsd` daemons and guest mounts, real `swtpm`, `vm_gpu="gl"` (virgl), disk and network throughput, Windows installations with the virtio drivers, and the uid 1000 drop under real host root (the tests run unprivileged, so the drop is checked by argument composition only).


