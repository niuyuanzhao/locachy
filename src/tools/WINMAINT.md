# Maintenance Windows Tail Partition

[中文](WINMAINT.zh.md)

`locachy-winmaint` provisions and manages an optional, reclaimable bare-metal
Windows partition triplet at the tail of the disk (or on a chosen secondary
disk). It is designed for occasional vendor firmware updaters, hardware
diagnostics, and Windows-only utilities without turning the machine into a
fragile dual-boot setup or weakening LOCachy's Secure Boot and TPM disk
sealing.

## Partition layout

A standard LOCachy GPT disk contains `p1` (LOCachy ESP, 512 MiB, FAT32 label
`LOCACHY_EFI`), optional `p2` (swap), and the LOCachy root partition (`p3`, or
`p2` without swap: ext4 `locachy-root`, optionally inside LUKS2
`locachy-crypt`).

Maintenance Windows appends three partitions at the tail of the disk, identified
both by GPT partition names and by recorded GUIDs in
`/var/lib/locachy/winmaint/state.json`:

| Slot | GPT name | Size | GPT type GUID | Purpose |
| --- | --- | --- | --- | --- |
| ESP | `LOCACHY_WIN_ESP` | 128 MiB | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | Isolated FAT32 ESP (`WINMAINT_EP`) for Windows Boot Manager only; `\EFI\BOOT\BOOTX64.EFI` is always stripped |
| MSR | `LOCACHY_WIN_MSR` | 16 MiB | `E3C9E316-0B5C-4DB8-817D-F92DF00215AE` | Microsoft Reserved partition (zeroed) |
| Windows | `LOCACHY_WIN_C` | 40–4096 GiB (default 48 GiB) | `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | Windows `C:` NTFS volume |

Process 1 (`init.c`) collects all FAT32 partitions during boot and selects the
ESP carrying `LOCACHY_EFI` and `/efi/EFI/locachy/locachy.efi`, so the secondary
`LOCACHY_WIN_ESP` partition never shadows LOCachy's `/efi`.

On multi-disk systems where kernel enumeration order can swap across boots
(such as `/dev/nvme0n1` and `/dev/nvme1n1`), `status`, `vm`, `boot-once`,
`boot-default`, `boot-guard`, `sanitize-esp`, and `reclaim` read every whole disk and use the one
carrying the recorded `disk_guid`, whatever device name it has now. If no disk
carries it, or two do (for example, a raw clone attached at the same time),
`locachy-winmaint` refuses to operate.

Choosing the disk: `locachy-winmaint status --json` lists `candidate_disks`
(each whole GPT disk that is not the live medium: `path`, `model`, `serial`,
`size_bytes`, `is_root`, `free_tail_gib`, `disk_guid`,
`has_maintenance_windows`), and reports `resolved_disk` and `on_root_disk` of
the configured Maintenance Windows. When run without raw block-device read
permission (for example from unprivileged `settings`), `status` falls back to
`lsblk` GPT metadata read-only; privileged mutation commands (`create`, `vm`,
`reclaim`, `boot-once`, `boot-default`, `boot-guard`, `sanitize-esp`) always read on-disk GPT
headers directly. `create --disk PATH` places the triplet on
that disk (the root disk by default). `create --root DIR --disk PATH` records
`DIR/var/lib/locachy/winmaint/state.json` for a system being installed; `--disk`
is then required and no root partition is ever shrunk. When `--disk` holds the
root partition of `DIR` (the installer's `--winmaint-disk` on the target disk),
the triplet is recorded with `on_root_disk` and that root's `root_part_uuid`, as
for this system, so `reclaim` grows the root into the space it frees. `DIR` must be a
mount point of that root (found with `findmnt`); otherwise a warning says the root was
not found and `--disk` is recorded as another disk. A disk
without enough free tail space is refused; `--root` never shrinks a root
partition, so only a free tail counts there.

When installing with `locachy-install --winmaint-disk`, the target must be a
writable whole disk with 512-byte logical sectors (the GPT parser works in
512-byte sectors), not on the live medium or running system, and not shared
with a Btrfs `array` volume. The reserved tail is `winmaint_gib * 1024 + 256`
MiB. On the system disk, partitioning reserves that tail at the end. On a
secondary disk formatted for an extra volume (`mode=format`), `locachy-storage`
runs in the `extra-volumes` stage with `--reserve-tail-mib` (leaving at least
1 GiB for the volume plus GPT overhead). On an unformatted secondary disk, it
must be GPT or blank (an existing raw filesystem, MBR/unknown partition table,
or existing Maintenance Windows partitions are refused) with at least that tail
unallocated after the last partition (gaps between partitions do not count).
Before anything is written, the installer also refuses a `--disk` or `--format-root`
target that holds (or is) a Maintenance Windows partition, and an extra volume on a disk
that holds them (formatted or in an array) or on such a partition (attached).
The `winmaint` stage runs after `extra-volumes`, `boot`, `secure-boot-enroll`,
and UEFI registration, calling `locachy-winmaint create --root DIR --disk PATH
--size-gib N`; on a resumed installation, once Maintenance Windows is recorded
in the target it is kept and cannot be changed. Afterward, `locachy-storage`
recognizes Maintenance Windows partitions (by GPT name or recorded `state.json`
PARTUUID) and refuses to format or put a disk holding them into an array,
refuses to attach them (or a whole disk holding them), and refuses to wipe them
(unrelated partitions on a non-system disk may still be attached).

## Creation and zero-reboot reclaim

### Create (`locachy-winmaint create --size-gib N [--disk /dev/...]`)

Minimum size is 40 GiB (default 48 GiB). When shrinking the LOCachy root, at
least 8 GiB of free space must remain on `locachy-root`.

- **Online (zero reboot):** if the target disk is a secondary disk or the root
  disk already has sufficient unallocated tail space, `locachy-winmaint`
  creates the three tail partitions with `sfdisk --append`, formats
  `LOCACHY_WIN_ESP` as FAT32, zeroes `LOCACHY_WIN_MSR`, and writes
  `/var/lib/locachy/winmaint/state.json` immediately.
- **One-shot offline shrink (one reboot):** when `locachy-root` fills the root
  disk, `locachy-winmaint` validates ext4 usage (`dumpe2fs` and `resize2fs -P`)
  and writes a plain `key=value` shrink plan to
  `/var/lib/locachy/winmaint/shrink-plan.conf` inside the root filesystem
  (encrypted inside LUKS2 when full-disk encryption is active, never on
  unencrypted `/efi`). `locachy-boot` detects the armed plan and temporarily
  packs `e2fsck`, `resize2fs`, `sfdisk`, and `partx` into that boot's
  initramfs. On the next cold boot (never on hibernation resume), process 1
  verifies the root `PARTUUID`, runs `e2fsck -f -p`, `resize2fs`, `cryptsetup
  resize` (if LUKS2), shrinks the root partition and appends the tail triplet,
  then consumes `shrink-plan.conf`. After `switch_root`,
  `locachy-winmaint finalize-create` formats `LOCACHY_WIN_ESP`, saves
  `state.json`, and runs `locachy-boot` to restore the normal lean initramfs.

### Reclaim (`locachy-winmaint reclaim`)

Reclaiming is completely online and requires no reboot:

1. Acquires the exclusive lock `/run/locachy/winmaint.lock` (refuses while the
   VM is running or any tail partition is mounted).
2. Removes the Maintenance Windows UEFI entry and any `BootNext` pointing to it.
3. Verifies disk GUID, all three partition GUIDs/types/names against
   `state.json`, and confirms the triplet sits at the disk tail.
4. Deletes the three tail partitions with `sfdisk --delete`.
5. If on the root disk immediately after `locachy-root`, grows the root GPT
   partition (`sfdisk -N`), notifies the kernel (`partx -u`), runs online
   `cryptsetup resize locachy-crypt` (when LUKS2), and runs online `resize2fs`.
6. Removes `state.json` and virtual OVMF variable state, and ensures `LOCachy`
   remains first in `BootOrder`.

## In-OS installation and updates in QEMU/KVM

`locachy-winmaint vm [--iso Win11.iso] [--virtio-iso virtio-win.iso] [--memory 4096] [--cpus 4] [--disk-bus nvme|virtio] [--nic e1000e|virtio] [--share-dir PATH[:TAG]]`
installs and updates Windows inside LOCachy without exposing LOCachy's own
partitions:

- **Isolated `dm-linear` virtual NVMe disk (`/dev/mapper/locachy-winmaint-disk`):**
  a pure-Python GPT builder creates a 1 MiB primary header/table image and a
  1 MiB backup GPT image carrying the **exact physical Disk GUID** and the
  **exact physical Partition GUIDs** of `LOCACHY_WIN_ESP`, `LOCACHY_WIN_MSR`,
  and `LOCACHY_WIN_C`, mapped as five `dm-linear` segments
  (`head + p4 + p5 + p6 + tail`). Zero sectors of LOCachy `p1`/`p2`/`p3` are
  mapped into the guest. Because Windows BCD identifies partitions by
  `(DiskGUID, PartitionGUID)` rather than LBA offset, BCD written inside the VM
  boots directly on bare metal.
- **Hyper-V enlightenments and `io_uring` storage (`stornvme.sys` default):**
  QEMU enables the explicit Hyper-V enlightenment set (`hv-relaxed`,
  `hv-vapic`, `hv-spinlocks=0x1fff`, `hv-time`, `hv-vpindex`, `hv-synic`,
  `hv-stimer`, `hv-stimer-direct`, `hv-reset`, `hv-frequencies`, `hv-tlbflush`,
  `hv-ipi`; not `hv-passthrough` or `hv-default`). The virtual disk uses
  `cache=none,aio=io_uring` on an emulated NVMe controller (`--disk-bus nvme`,
  the default, `-device nvme,drive=disk0,serial=locachy-winmaint`) so VM and
  bare-metal boot share Windows' in-box `stornvme.sys`. `--disk-bus virtio`
  switches the disk to `virtio-blk-pci` with a dedicated `iothread` and
  `discard=on` (requires `viostor`). Bare-metal NVMe boot after a `virtio-blk`
  VM run is guarded by `locachy-storage.ps1` (below), and is **unverified on
  Windows hardware**; `nvme` remains the safe default.
- **Networking, display, RNG, and `virtiofs` sharing:**
  - `--nic e1000e` (default, in-box Windows driver) or `--nic virtio`
    (`virtio-net-pci`, requires `NetKVM`); the host backend uses `passt` when
    `passt` is on `PATH` and QEMU supports `-netdev passt`, falling back to
    user-mode NAT (`-netdev user`).
  - `-device virtio-rng-pci` supplies host entropy; `-vga none -device
    virtio-vga` is added when the host QEMU provides `virtio-vga` (Arch's
    `qemu-desktop` / `qemu-hw-display-virtio-vga`), otherwise QEMU's standard
    VGA is kept. Windows guest behavior under `virtio-vga` is **unverified**.
  - `--share-dir PATH[:TAG]` (repeatable) shares host directories over
    `virtiofs` (`memory-backend-memfd,share=on` + `vhost-user-fs-pci` +
    host `virtiofsd`; requires `viofs` / WinFsp in Windows, PnP-dormant on
    bare metal; guest mounting is **unverified**).
- **QMP control socket and Wayland display:** when `--qmp-sock PATH` or
  `LOCACHY_VM_QMP_SOCK` is set, `locachy-winmaint vm` exposes QEMU's QMP monitor
  at that unix socket (and cleans it up on exit); `locachy-winmaint vm-stop`
  sends `system_powerdown` (optionally followed by `quit` after `--wait`) for
  clean shutdown. On a Wayland session, GTK display runs with `gtk,gl=off`. Every GTK
  display has `window-close=off`: closing the Maintenance Windows window never
  quits QEMU (the bar's middle-click, the overview's close, the window's own
  button), so Windows' power is never pulled with NTFS on the real partition
  left dirty. Stop Windows from the Home screen (its QMP stop) or shut it down
  inside Windows.
- **Private OVMF and emulated TPM 2.0:** QEMU boots with read-only `OVMF_CODE`
  and a private `/var/lib/locachy/winmaint/ovmf-vars.fd`, plus `swtpm` TPM 2.0
  emulation, so Windows Setup and Windows Update never touch physical UEFI
  NVRAM.
- **Unattended hardening seed (`seed.img`):** when `--iso`, `--seed`, or
  `--virtio-iso` is passed, a read-only MBR-partitioned FAT removable USB disk
  (`usb-storage,removable=on`; 16 MiB FAT16 without drivers, FAT32 when
  `--virtio-iso` stages `$WinPEDriver$\viostor` and `$WinPEDriver$\NetKVM` for
  `--virtio-os w11|w10`, and also attaches the ISO as a second CD-ROM) supplies
  `autounattend.xml` and post-setup scripts that:
  - skip online Microsoft account OOBE prompts (`BypassNRO`),
  - disable automatic BitLocker / Device Encryption (`PreventDeviceEncryption=1`),
  - disable hibernation and Fast Startup (`powercfg /h off`, `HiberbootEnabled=0`),
  - set the hardware RTC to UTC (`RealTimeIsUniversal=1`),
  - install the Windows-side scripts under
    `%SystemRoot%\System32\LOCachy`, including the storage guard, the
    Windows-first-aware repair script, and the Return to LOCachy script and
    public desktop shortcut;
  - register `LOCachyStorageBoot` as a SYSTEM startup task with battery
    start/stop restrictions disabled. It checks the firmware boot order and
    clears `StartOverride` and sets `Start=0` on `stornvme` and `viostor`;
  - register a synchronous Local Group Policy computer shutdown script
    (`Machine\Scripts\Shutdown\locachy-shutdown.cmd`) to repeat only the
    storage step. Registration removes the old Event 1074 task, which Task
    Scheduler can terminate during shutdown.
- **Installation media from `locachy-winiso`:** when `vm` gets no `--iso` (or
  no `--virtio-iso`) and Windows is not yet installed on `LOCACHY_WIN_ESP` (no
  `\EFI\Microsoft\Boot\bootmgfw.efi`, read with mtools), it asks
  `locachy-winiso path windows` (or `path virtio-win`) when that command exists
  next to `locachy-winmaint` or in `/usr/local/bin`. Contract: one absolute path
  on stdout and exit 0; exit 3 when the ISO is not available. Exit 3, any other
  answer (warned on stderr), or no such command attaches nothing, as before.
  An ISO given by `--iso` or `--virtio-iso`, or named by `locachy-winiso`, must
  be a regular file: directories and devices are refused (a root QEMU reads them).
  Options given on the command line always win, and once Windows is installed
  nothing is attached unasked. `create` takes no ISO, so it asks nothing.

## Driver seed for other Windows VMs (`driver-seed`)

`locachy-winmaint driver-seed --virtio-iso PATH --out IMG [--virtio-os w11|w10]`
builds a small MBR-partitioned FAT16 image (about 30 MiB for virtio-win 0.1.302)
whose partition root holds only `$WinPEDriver$\viostor` and
`$WinPEDriver$\NetKVM`, the amd64 folders of the chosen Windows tree, staged by
the same code as the seed's. `PATH` is a virtio-win ISO (a regular file; block
devices and directories are refused). It needs no root (bsdtar, sfdisk,
mkfs.fat, mtools), replaces `IMG` as a whole, and prints only the image's
absolute path on stdout (exit 0; errors go to stderr with exit 1). Windows Setup
loads drivers from `$WinPEDriver$` on any removable disk, so `locachy-app`
attaches the image read-only as `usb-storage,removable=on` to custom Windows VMs.

## Four-layer boot protection

1. **Layer 1 (no fallback hijacking on `p4`):** `LOCACHY_WIN_ESP` never keeps
   `\EFI\BOOT\BOOTX64.EFI`. `locachy-winmaint sanitize-esp` strips it after
   every VM exit, before every `boot-once`, and on LOCachy boot via
   `boot-guard`.
2. **Layer 2 (virtual NVRAM for updates):** Windows installation and Windows
   Updates run in QEMU/KVM against private `ovmf-vars.fd`, leaving motherboard
   NVRAM untouched.
3. **Layer 3 (one-shot `BootNext` only):** `locachy-winmaint boot-once` refuses
   while the VM lock is held, verifies `p4:\EFI\Microsoft\Boot\bootmgfw.efi`
   exists, registers `LOCachy Maintenance Windows` via
   `locachy-efi --bootnext-entry` (`efibootmgr --create-only`, never placed in
   `BootOrder`), and sets one-shot `BootNext`. Firmware consumes `BootNext`
   when it starts Windows and uses the regular `BootOrder` for later boots.
4. **Layer 4 (self-healing `BootOrder`):** `locachy-efi --ensure-first` (invoked
   by `locachy-winmaint heal-boot` and `boot-guard`) restores `LOCachy` to the
   front of `BootOrder` and removes any Maintenance Windows entry from
   `BootOrder`. On the Windows side, `LOCachyStorageBoot` performs the
   matching BCDEdit repair after Windows boots. While Windows is set to start
   first (next section), both sides preserve that order. The Windows shutdown
   hook repairs storage settings only.

## Windows first until switched back (`boot-default`)

For installing drivers on bare metal, where Windows restarts many times,
`boot-once` is not enough. `locachy-winmaint boot-default windows [--reboot]`
makes Windows start first on every restart until it is switched back:

- After the same checks as `boot-once` (lock free, `bootmgfw.efi` present,
  `\EFI\BOOT\BOOTX64.EFI` stripped), `locachy-efi --windows-first` creates or
  reuses a `LOCachy Maintenance Windows (default)` entry for
  `p4:\EFI\Microsoft\Boot\bootmgfw.efi` and puts it first in `BootOrder`.
  `LOCachy` (and `LOCachy (Previous)`) follow right behind it and are never
  removed; other entries on the Maintenance Windows ESP leave `BootOrder`.
  LOCachy stays one pick away in the firmware boot menu.
- Only then is the mode recorded in `state.json`: `"boot_default": "windows"`
  and `"boot_default_boot_id"` (this boot's
  `/proc/sys/kernel/random/boot_id`). A failed firmware write records nothing.
  `--reboot` then restarts through process 1.
- `status --json` reports `boot_default` (`"windows"` or `"locachy"`),
  `boot_default_pending_reboot` (set in this boot: Windows comes at the next
  restart) and `boot_default_overridden` (LOCachy is running in a later boot,
  so it was picked in the firmware boot menu); plain `status` prints the same.
- The mode is "the Windows-first entry precedes `LOCachy` in the firmware boot
  order", on both sides. While it is recorded, `boot-guard` (LOCachy's
  boot-time check) and `heal-boot` call `locachy-efi --ensure-first
  --keep-windows-first`, which keeps that order. When the entry no longer
  precedes `LOCachy`, it puts `LOCachy` first as usual and exits with 3;
  `locachy-winmaint` then deletes the entry and clears the mode. `boot-once`
  arms nothing while the mode is set, because Windows starts first anyway.
- `locachy-winmaint boot-default locachy` switches back from LOCachy:
  `--ensure-first` without the keep flag (LOCachy first, Maintenance Windows
  entries out of `BootOrder`), the `(default)` entry deleted
  (`--remove-entry --label "LOCachy Maintenance Windows (default)"`), the mode
  cleared. `reclaim` removes the entry with the others on that ESP.
- **Windows side.** `locachy-win-shutdown.ps1` and `locachy-return.ps1` read
  `bcdedit /enum firmware /v` by block structure and invariant values, never by
  localized field labels (English `identifier` / `displayorder` / `description`,
  Chinese `标识符` / `显示顺序` / `描述`, and so on): blocks are split by blank lines
  or `---` underlines; a block's identifier is its first line ending in a `{GUID}`,
  with `{fwbootmgr}` recognized both by that name and by its canonical GUID
  `{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}`; `{fwbootmgr}`'s `displayorder` is the
  first key after its identifier, and the indented GUID lines after it are its
  entries; and an entry is identified by a value that equals exactly one of the
  ASCII descriptions `LOCachy`, `LOCachy Maintenance Windows`, or
  `LOCachy Maintenance Windows (default)`, which `locachy-efi` writes into NVRAM
  and Windows does not localize. While an entry described as
  `LOCachy Maintenance Windows (default)` precedes `LOCachy`
  in that display order, the guard changes nothing in the boot order and only runs the
  storage step. The **Return to LOCachy** shortcut on the all-users desktop runs
  `locachy-return.ps1`, which elevates itself (UAC), finds the entry described
  as `LOCachy`, runs `bcdedit /set {fwbootmgr} displayorder <id> /addfirst`,
  takes the `(default)` entry out of the display order
  (`bcdedit /set {fwbootmgr} displayorder <id> /remove`), runs the guard once
  (it is armed again), and offers to restart (`shutdown.exe /r /t 0`).
  Microsoft documents this form of the command in
  [BCD System Store Settings for UEFI](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi)
  (`Bcdedit /set {fwbootmgr} displayorder {bootmgr} /addfirst`), and the
  `/displayorder` option in
  [BCDEdit Command-Line Options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdedit-command-line-options)
  ("Specifies the display order that the boot manager uses when displaying boot
  options to a user"). The `/remove` form is the one the guard has always used;
  Microsoft's pages above do not document it. At its next boot LOCachy's
  `boot-guard` sees the release and clears the mode. The shortcut and the new
  scripts are written by the seed's first-logon script, so a Windows installed
  before this change does not get them.
- **Settings → System → Maintenance Windows**, after "Boot Windows once":
  "Start in Windows by default (until switched back)" shows the mode
  (`[OFF] LOCachy starts first`, `[ON] Windows from the next restart`,
  `[ON] Windows starts first`, or that LOCachy was picked in the firmware menu).
  Enter twice turns it on, one Enter switches back. "Reboot into Windows now"
  (Enter twice) sets the mode and restarts. Both go through the system job
  `winmaint-boot-default` (`windows`, `windows --reboot` or `locachy`), the way
  `winmaint-boot-once` does.

## Secure Boot and TPM compatibility

- Enroll Secure Boot with Microsoft third-party UEFI keys alongside LOCachy's
  own keys (`locachy-tpm secure-boot --enroll --microsoft`) so both LOCachy's
  signed UKI and Microsoft's signed `bootmgfw.efi` pass Secure Boot verification.
- LOCachy's own-key TPM 2.0 sealing binds LUKS2 to PCR 7 plus signed PCR 11
  (and never PCR 1). Arming one-shot `BootNext` does not invalidate TPM
  unlocking when returning to LOCachy.
- Bare-metal Windows runs with BitLocker / Device Encryption disabled so it
  never claims or re-provisions the physical TPM against LOCachy's LUKS2 keys.

## Configuration and Settings integration

The `rc.conf` key for the Settings row, and the default of `create` without
`--size-gib`:

- `winmaint_size_gib="48"` — preferred size (40–4096 GiB) of the tail Windows
  partition. The Windows VM's RAM (`vm --memory`, 4096 MiB) and vCPUs
  (`vm --cpus`, 4) are command-line options only; Settings has no row for them.

## Verification status (read first)

- Verified by the automated tests: the QEMU command line (CPU, storage, network,
  display, RNG, virtiofs, virtio-win CD), QEMU's acceptance of the argv with
  KVM (on a host with QEMU 11.1.2), the `virtiofsd` command line through a stub,
  the seed image layout (`$WinPEDriver$`, scripts), and the guest scripts' text.
- **Verified on QEMU/KVM with a real Windows 11 26H2 guest:** Setup used the
  removable driver seed and installed to `virtio-blk`. The same installed disk
  booted to the desktop on emulated NVMe with `e1000e`, without the installer
  ISO or seed. Both `stornvme` and `viostor` showed `Start=0` and no
  `StartOverride`. In Windows, the startup task restored LOCachy first after a
  VM reset; both battery restrictions on the task were disabled. A separate
  Windows restart preserved the intentional Windows-first entry. The Return
  to LOCachy script also restored LOCachy first against live BCDEdit output.
- **Storage repair at shutdown (Windows 11 26H2 VM):** with the startup task
  disabled, the normal shutdown script removed an injected
  `viostor\StartOverride\0=3`. A control script that only recorded its execution
  left the value intact. Both SYSTEM hives were read offline after clean guest
  shutdown and before another Windows boot.
- **Limit of the shutdown hook:** with the startup task disabled, the private
  OVMF `BootOrder` at the reset boundary still placed Windows Boot Manager
  first. The Local Group Policy script ran as Local System, but BCDEdit could
  not open the firmware BCD store with that service token. The shutdown hook
  therefore handles storage only. Startup repair takes effect only after
  Windows has booted; to choose LOCachy for the very next boot, use **Return to
  LOCachy** before restarting.
- **Not verified:** OMEN bare-metal NVMe boot and firmware behavior, operation
  on physical battery power, or mounting a `virtiofs` share from Windows.

## Automated tests

Run the rootless test suite on a Linux development host:

```sh
bash tools/test-winmaint.sh
```

With `/dev/kvm` and OVMF, `bash tools/test-winmaint-ovmf.sh RUNTIME_ROOT NEW_OUTPUT`
runs the boot protection's `efibootmgr` writes and `sanitize-esp` in real OVMF
firmware (private variables, FAT32 partitions of a throwaway disk); judge it by
its PASS lines.

It verifies the pure-Python GPT builder and `sfdisk -V` validity, exact Disk
and Partition GUID preservation across physical and virtual `dm-linear` layouts
with complete isolation of `p1`/`p2`/`p3`, online create/sanitize/reclaim on
sparse disk images, shrink-plan validation and the `init.c` C parser plus
multi-ESP disambiguation compiled with both `gcc` and `musl-gcc -static`,
`autounattend.xml` and seed scripts, mocked `locachy-efi` `BootNext`/`--ensure-first`
flows, exclusive lock enforcement, and `locachy-boot` lean vs shrink-armed
initramfs packing. It also checks that unprivileged `status` resolves disks and the root layout through `lsblk` when raw block reads are denied (mocked), and that privileged disk resolution stays raw-only.
For Windows first it checks the `BootOrder` transitions through `locachy-efi`
with a mocked `efibootmgr` (in `test-efi.py`: Windows first, kept, released by
Return to LOCachy, switched back, `LOCachy` never removed), `boot-default`'s
state, status fields and its guard/heal/boot-once behaviour, the generated
Windows scripts and shortcut (exact `bcdedit` commands), the `driver-seed`
image's contents, and `vm`'s `locachy-winiso` lookup against a stub.
