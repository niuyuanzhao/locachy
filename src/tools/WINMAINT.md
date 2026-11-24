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

`locachy-winmaint vm [--iso Win11.iso] [--memory 4096] [--cpus 4]` installs and
updates Windows inside LOCachy without exposing LOCachy's own partitions:

- **Isolated `dm-linear` virtual NVMe disk (`/dev/mapper/locachy-winmaint-disk`):**
  a pure-Python GPT builder creates a 1 MiB primary header/table image and a
  1 MiB backup GPT image carrying the **exact physical Disk GUID** and the
  **exact physical Partition GUIDs** of `LOCACHY_WIN_ESP`, `LOCACHY_WIN_MSR`,
  and `LOCACHY_WIN_C`, mapped as five `dm-linear` segments
  (`head + p4 + p5 + p6 + tail`). Zero sectors of LOCachy `p1`/`p2`/`p3` are
  mapped into the guest. Because Windows BCD identifies partitions by
  `(DiskGUID, PartitionGUID)` rather than LBA offset, BCD written inside the VM
  boots directly on bare metal.
- **Same storage driver (`stornvme.sys`):** the virtual disk is attached as a
  QEMU NVMe controller (`-device nvme,drive=nvme0,serial=locachy-winmaint`),
  matching bare-metal NVMe boot without driver-switch boot failures.
- **Private OVMF and emulated TPM 2.0:** QEMU boots with read-only `OVMF_CODE`
  and a private `/var/lib/locachy/winmaint/ovmf-vars.fd`, plus `swtpm` TPM 2.0
  emulation, so Windows Setup and Windows Update never touch physical UEFI
  NVRAM.
- **Unattended hardening seed (`seed.img`):** when `--iso` or `--seed` is
  passed, a read-only FAT12 USB disk supplies `autounattend.xml` and post-setup
  scripts that:
  - skip online Microsoft account OOBE prompts (`BypassNRO`),
  - disable automatic BitLocker / Device Encryption (`PreventDeviceEncryption=1`),
  - disable hibernation and Fast Startup (`powercfg /h off`, `HiberbootEnabled=0`),
  - set the hardware RTC to UTC (`RealTimeIsUniversal=1`),
  - install a Local Group Policy shutdown script and Event 1074 scheduled task
    (`C:\ProgramData\LOCachy\locachy-win-shutdown.ps1`) that keeps `LOCachy`
    first in firmware boot order.

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
   `BootOrder`), and sets one-shot `BootNext`. The next reboot automatically
   returns to LOCachy.
4. **Layer 4 (self-healing `BootOrder`):** `locachy-efi --ensure-first` (invoked
   by `locachy-winmaint heal-boot` and `boot-guard`) restores `LOCachy` to the
   front of `BootOrder` and removes any Maintenance Windows entry from
   `BootOrder`. On the Windows side, `locachy-win-shutdown.ps1` performs the
   matching `bcdedit /set {fwbootmgr} displayorder` repair on shutdown.

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
initramfs packing.
