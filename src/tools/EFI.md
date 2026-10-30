# UEFI boot entries

[中文](EFI.zh.md)

The installed system has a formal `LOCachy` UEFI entry for
`\EFI\locachy\locachy.efi`. `EFI/BOOT/BOOTX64.EFI` remains the removable-media
fallback, not the only normal boot route.

After writing both images, the installer runs the installed source's
`locachy-efi --esp TARGET --first`. It registers the selected ESP and puts
LOCachy first in BootOrder, keeping every other entry and its relative order.
An unavailable or unwritable firmware-variable store gives a visible warning;
the target images stay installed and the fallback is retained.

These changes are local, not yet published. On an existing system, the first
source-first update installs package tools before it finishes compiling LOCachy;
its earlier pacman hooks may still run the old boot helper. The new EFI helper
and registration path become available when that update completes `make.sh`.
No separate installation through the package-tools bootstrap is needed.

Every normal `locachy-boot` image update also checks the entry. It reuses an
exact matching active entry, activates an inactive one, or appends it to
BootOrder if absent. It does not keep moving LOCachy ahead of an order the user
chose. If no exact entry exists, it creates one. Old entries pointing to other
partitions, even those named LOCachy, are reported and left untouched. BootNext,
other operating systems and Secure Boot keys are never changed by this helper.
The installer's registration runs after the final ESP images and any requested
Secure Boot enrollment, before marking installation complete. Boot#### and
BootOrder may affect firmware measurements such as PCR 1; LOCachy's default
bindings do not use PCR 1 (own keys: PCR 7 plus signed PCR 11; TPM-only: 0+7;
TPM 1.2: 0). A custom PCR policy may require re-enrollment. Actual registration
under TPM/Secure Boot was not part of the OVMF test below.

To repair only the entry on an installed system, as root:

```sh
locachy-boot --register-only
```

This does not rebuild or replace an image. Unlike a normal image update, an
unsuccessful explicit repair exits nonzero. `locachy-efi --esp /efi --first`
explicitly makes it first again; normal updates never request that priority.

## Identity and failure rules

The helper uses standard `findmnt`, `lsblk --properties-by blkid`, and
`efibootmgr`; it never writes raw EFI variables. The target must be an explicit
FAT mount on a GPT EFI System Partition, its mount major/minor must match the
block node, its parent must be a disk, and its non-linked loader file must
exist. Partition number and PARTUUID come from block metadata, not from
trimming names, so SATA, NVMe and MMC naming all use the same path.

Only a matching label, GPT UUID, partition number and loader path is reused.
`efibootmgr --create-only` avoids changing BootOrder as a side effect; the
helper then writes exactly the intended order and rereads firmware state.
Partial failure is retryable: an entry created before an order-write failure
is reused by the next call. Legacy boot, absent/read-only efivarfs, unsupported
targets, command errors and firmware refusing to retain state are all explicit
failures. On a normal image update these do not discard the successfully
updated UKI or fallback. A normal image update also checks that `/efi` is an
actual FAT mount before touching ESP files: a stale directory on the root
filesystem is not treated as a mounted ESP.

## Tests

`python3 -B tools/test-efi.py SCRATCH_PARENT` passes 15 tests covering target identity, SATA/NVMe/MMC,
parsing both File(PATH) and bare file device paths, idempotence, inactive/order
repair, wrong targets/labels, failures and integration order with doubles.

On the Linux development server, the isolated real test is:

```sh
bash tools/test-efi-vm.sh RUNTIME_ROOT PACKAGE_CACHE NEW_OUTPUT
```

It boots a diskless 2-vCPU/2-GiB OVMF guest with private NVRAM and a disposable
128-MiB GPT ESP. Runtime/source/cache are read-only; cached ukify/pefile packages
are extracted only in the guest overlay. No host firmware, display, input or
network is exposed. It actually writes and rereads Boot#### and BootOrder,
checks installation priority, repeat calls without duplicates, inactive/missing
order repair, deletion followed by `--register-only`, and an actual boot-image
rebuild preserving an intentionally changed order.

The passing run is `~/x230-dev/efi-agent/vm-guard/` (`EFI_VM_STATUS=0`, five
functional PASS checks and `PASS EFI VM complete`). Logs and before/after NVRAM
text are retained. This tests real efibootmgr against OVMF, not a real machine's
firmware or a reboot through the newly registered image.

Reference: [efibootmgr(8)](https://man.archlinux.org/man/efibootmgr.8.en).
