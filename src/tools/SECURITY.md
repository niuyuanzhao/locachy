# Disk encryption, the TPM and Secure Boot

[中文](SECURITY.zh.md)

This file covers the encrypted disk, how the TPM opens it, Secure Boot with
the machine's own keys, hibernation, and fwupd's host security report. The
lock screen and authentication are described in the old tree's
SECURITY.md (`distro/src/tools/SECURITY.md`). They have not yet been
reviewed against this tree.

## What there is

**The disk.** The installer makes a LUKS2 root (`locachy-crypt`) and a
LUKS2 swap (`locachy-swap-crypt`), both opened by the same long passphrase.
The passphrase is never removed. It is asked whenever the TPM does not open
a disk.

There is no PIN anywhere. The user decided that the passphrase is long
enough as the only typed secret.

**TPM 1.2** (ThinkPad X230): `locachy-tpm enroll` seals a random key to PCR
0 with `TPM_Seal` and stores the blob in a `locachy-tpm12` LUKS2 token.
This is unchanged.

**TPM 2.0, without this machine's own Secure Boot keys**: unchanged too.
`systemd-cryptenroll --tpm2-pcrs=0+7` seals the key to the firmware (PCR
0) and to Secure Boot's state and keys (PCR 7). `locachy-tpm` adds
`--tpm2-public-key=`. Otherwise `systemd-cryptenroll` would add a signed
PCR 11 policy by itself whenever `/etc/systemd/tpm2-pcr-public-key.pem`
exists.

**TPM 2.0, with this machine's own Secure Boot keys**: this follows the
Arch Wiki, systemd and sbctl.

- `locachy-tpm secure-boot` makes the keys with standard tools:
  `sbctl create-keys` for PK, KEK and db, and `ukify genkey` for the PCR
  signing key pair in `/etc/systemd/`.
- `locachy-boot` builds the UKI with
  `ukify --signtool=systemd-sbsign --secureboot-private-key=… --secureboot-certificate=… --no-sign-kernel`.
  It also adds
  `--pcr-private-key=… --pcr-public-key=… --phases=enter-initrd`, so the
  image carries the PCR 11 value it measures up to `enter-initrd`, signed
  with the PCR key (`.pcrsig`, `.pcrpkey`). The initramfs has
  `/etc/initrd-release`. With it, systemd's TPM token finds the stub's
  `/.extra/tpm2-pcr-signature.json`.
- `locachy-tpm secure-boot --enroll [--microsoft]` runs `sbctl
  enroll-keys`, which includes sbctl's own option-ROM check (it reads the
  TPM event log in securityfs). This works only in setup mode. It runs only
  after the image the firmware starts (`EFI/locachy/locachy.efi` and
  `EFI/BOOT/BOOTX64.EFI`) is byte-for-byte the signed one just made.
  Otherwise the machine would not start once Secure Boot is on.
- The disk binding is
  `systemd-cryptenroll --tpm2-pcrs=7 --tpm2-public-key=… --tpm2-public-key-pcrs=11 --wipe-slot=tpm2`.
  The old TPM 2.0 slot is removed only after the new one is in.
- Process 1 measures `enter-initrd` before it opens any disk and
  `leave-initrd` before it leaves the initramfs or resumes a hibernated
  system. It uses `systemd-pcrextend`, as `systemd-pcrphase-initrd.service`
  does, and mounts efivarfs for it. The key therefore opens only inside a
  signed image's initramfs, never in the running system.
- Enrolling the keys changes PCR 7. The disks that were bound get a
  key slot of their own: a random 256-bit key, kept in
  `/var/lib/locachy/tpm-pending` on the encrypted root. The slot is added
  with the existing TPM key when the TPM still opens the disk in the
  running system (the PCR 0+7 binding does), or else with the passphrase.
  At the next start, the old binding fails and the passphrase is asked
  once. Process 1 then runs `locachy-tpm enroll --pending`, which binds the
  disk to the new PCR 7 and the signed policy and removes the slot and its
  key. The installer uses the same mechanism (`prepare`), because the live
  system's PCR 7 is not the installed system's. Settings shows "bound at the
  next start …" while a binding waits.
- Pending binding survives failures: the random key and UUID association are
  durably saved before adding a temporary LUKS slot. A retry checks whether
  that key already opens a slot instead of adding duplicates. Missing disks,
  binding failures and failed slot removal keep that disk's request and key;
  successful disks can finish separately. Once enrollment succeeds, it is
  recorded so a cleanup retry does not need to enroll again. Keys are removed
  only after slot removal is confirmed (or the slot is already absent).
  Malformed requests fail explicitly and stay untouched. These files use
  root-only modes, same-directory atomic replacement and file/directory fsync.

**Hibernation.** Process 1 opens the swap first, with its TPM token, and
nothing is typed. Before resuming an image, the root must also have been
successfully unlocked, either by its actual TPM token or by the long passphrase.
Token metadata in the LUKS header is not authentication and cannot authorize
resume by itself. Root and swap are unlocked before `leave-initrd`; successful
TPM-only resume still asks for nothing and keeps the session lock. Failed or
forged root tokens fall back to the passphrase, and failed passphrase attempts
never release a root key from the hibernation image. Each start's `swapon` and each
shutdown discard the whole swap (`SWAP_FLAG_DISCARD`, `DISCARD_ONCE`;
`cryptsetup --allow-discards`). A resumed image does not stay in the swap.

**fwupd's Host Security ID.** It is the output of `fwupdmgr security`. The
installer shows the HSI and the checks that failed at the encryption step,
which needs fwupd on the live medium (`installer/packages.x86_64`).
Settings shows the HSI under Security, Host Security. Enter there runs
fwupd's whole report in a terminal, with the fixes fwupd offers.

## Audit round 1 (2026-10-29)

"Own keys" means TPM 2.0 bound with this machine's Secure Boot keys.
"PCR 0+7" means TPM 2.0 without them, as before. "TPM 1.2" is the X230's
binding.

| Attack | Own keys | PCR 0+7 | TPM 1.2 |
|---|---|---|---|
| Disk taken out of the machine | Stopped: the sealed key opens only in this TPM. | Stopped. | Stopped. |
| Live USB or another OS started on the machine | Stopped. With Secure Boot on, only images signed with this machine's db key start, or Microsoft-signed ones with `--microsoft`. A Microsoft-signed system (shim, a Windows boot manager) records the Microsoft CA as the authority in PCR 7, and shim adds its MOK variables, so PCR 7 differs. | **Not stopped.** Any system started with Secure Boot off reads the same PCRs 0 and 7 and can unseal the key (`systemd-cryptenroll --unlock-tpm2-device`, tpm2-tools). | **Not stopped**, the same with PCR 0. |
| Secure Boot turned off, keys cleared or replaced in the firmware | Stopped: PCR 7 measures SecureBoot, PK, KEK, db and dbx. The passphrase is asked. | The binding is to that state already (see above). | Not measured. |
| Changed boot image on the ESP (evil maid) | Stopped: an unsigned or differently signed image does not start. One signed with this db key but not this PCR key gets no matching PCR 11 signature. | **Not stopped**: the image is unsigned, and the replaced image unseals the key itself. | **Not stopped.** |
| Command line or extra initrd from a loader | Stopped: with Secure Boot on, systemd-stub ignores command-line overrides (the UKI has `.cmdline`). No signed loader is there to pass an initrd. | **Not stopped**: with Secure Boot off, `initrd=` and `rdinit=` reach the kernel. | **Not stopped.** |
| An older signed image (downgrade) | **Not stopped.** Every image ever signed with this PCR key and db key opens the disk; `locachy-prev.efi` is one of them, on the ESP. A signed policy has no revocation. This matters if an older image has a flaw before `leave-initrd`. Remedy, not automated: a new PCR key (`ukify genkey`, then `locachy-tpm enroll`) invalidates all older images. | n/a | n/a |
| Root on the running system | Cannot unseal after `leave-initrd`. It can sign images with the PCR and db private keys on the encrypted root (`/etc/systemd`, `/var/lib/sbctl`), so a compromised root can leave a signed backdoor image. | Can unseal at any time; it already has the disk. | Can unseal. |
| Stolen swap (disk out) | Stopped, as the root. | Stopped. | Stopped. |
| Hibernated machine taken | The swap opens with nothing typed. The session comes back at the lock screen, which guards it, as after a TPM-only cold start. `leave-initrd` is measured first, so the restored system cannot unseal. The kernel runs with `intel_iommu=on` against DMA. | As own keys, without the `leave-initrd` effect. | As PCR 0+7. |
| Swap bound to the TPM, root to its passphrase only | Fixed in this round. Before, process 1 resumed the TPM-opened swap without the root's passphrase, so the image gave the root's key away. It now requires an actual successful root unlock before any encrypted resume. Merely adding unauthenticated TPM-token JSON to the root header cannot bypass the passphrase. | Same. | Same. |
| Stale hibernation image or swapped-out pages | Discarded at each start and each shutdown on disks with discard. **Not removed** on disks without discard (hard disks), where they stay encrypted with the swap's key. The SSD may keep physical copies after TRIM. Discards through dm-crypt show which blocks are used (the cryptsetup FAQ's trade-off), for the swap only. | Same. | Same. |
| TPM bus sniffing (discrete TPM) | Passive sniffing: stopped. systemd salts the unseal session with the SRK recorded at enrollment, and the secret crosses the bus encrypted. **Not stopped**: an active interposer (TPM reset and replayed PCR values, a fake TPM). | Same. | **Not stopped**: TPM 1.2 returns the key in the clear over LPC. |
| Firmware rewritten (no Boot Guard on the X230) | **Not stopped**: the firmware is the root of trust and can fake measurements. | **Not stopped.** | **Not stopped.** |
| Microsoft keys enrolled (`--microsoft`) | The disk stays closed to Microsoft-signed systems (PCR 7, above). sbctl enrolls no dbx, so revoked Microsoft binaries still **start**. That breaks only the claim that LOCachy alone starts, not the disk. | n/a | n/a |
| Recovery path | The passphrase slot always stays. It is asked whenever the TPM refuses, with what happened shown in red. The waiting binding's slot uses a 256-bit random key kept inside the encrypted root, so it adds no way in from outside. It is removed once the binding is done, by `enroll`, or by `clear`. | Same. | Same. |
| No lock at boot with TPM unlock | Refused together, in the installer and in settings. | Same. | Same. |

Also fixed in this round:

- A TPM 2.0 binding made without own keys while a PCR key existed got a
  signed PCR 11 policy anyway, because systemd-cryptenroll loaded the key
  by itself. It is now `--tpm2-public-key=`.
- Enrolling the keys with no signed image on the ESP would have left a
  machine that does not start. It is refused now.
- Re-binding removed the old TPM token first. If the new enrollment
  failed, the TPM unlock was gone. The old token now goes only after the
  new one is in.
- When a hibernation image did not come back, only the passphrase was
  offered. The token is tried first; a PCR 0+7 binding still opens.

Open:

- Downgrade to older signed images (above).
- Hibernation on kernels whose lockdown blocks it under Secure Boot. Arch
  and CachyOS kernels do not lock down by themselves; the VM shows what this
  kernel does.
- Discard-less disks keep stale swap contents, encrypted.

## Audit round 2: pending-binding failure recovery

Review found that the first pending-binding implementation discarded recovery
state when a disk was temporarily absent or slot removal failed. Saving the
request after adding the slot also left an interruption window with no saved
UUID association. These are reliability defects in enrollment, not a reason to
remove the long recovery passphrase or weaken the measured policy.

The implementation now saves the key/association before changing the header,
retains failed devices, records completed enrollment separately from cleanup,
and retries slot retirement safely. The helper and actual TPM 2.0 VM tests
below cover the reported-failure windows. They do not prove behavior under
arbitrary storage corruption or replace a hardware enrollment test.

## Tests

- `tools/test-tpm.py`: locachy-tpm's rules with doubles for the firmware's
  variables, the LUKS tools and the TPM. It checks:
  - own keys means this db certificate in db with Secure Boot on;
  - the own-key binding (PCR 7, the signed PCR 11 policy, the old slot
    wiped after, no PIN);
  - the PCR 0+7 binding without the PCR key, and `--pcrs`;
  - the waiting binding: key slots, modes, waiting without Secure Boot,
    binding with the key file, removal;
  - `secure-boot --enroll`: keys made, the image on the ESP first, the
    TPM's own key first then the passphrase, and no binding left waiting
    when sbctl refuses;
  - `status`;
  - failure/retry with a missing disk, failed slot removal, interrupted
    prepare/retire, atomic-save failure, header I/O error and malformed request.
    The helper suite has 16 tests.
- Real TPM 2.0/LUKS failure/retry: `~/x230-dev/tpm-agent/runs/pending2/serial.log`
  records four PASS checks for a header written before a reported failure,
  reuse without duplicate slots, a hidden swap UUID plus failed root-slot
  removal, and successful retry with both recovery passphrases retained.
  `runs/pending-boot/serial.log` then records TPM auto-unlock after the retry.
  The real tools run inside the disposable VM; wrappers inject the reported
  failure and missing-device condition. Actual power loss and filesystem
  faults were not simulated; atomic-write interruption windows use doubles.
- `tools/test-init-unlock.py` compiles the actual init unlock/resume functions
  with block, TPM and UI doubles. Seven checks cover absent/forged root tokens,
  successful token unlock, passphrase fallback, exhausted attempts, unidentified
  encrypted root and unchanged unencrypted/cold starts. No real hibernation is
  performed by this authentication regression.
- `tools/test-install.py`: the installer's advice, `--secure-boot`
  checks, fwupd's report, and the order (keys made before the image,
  enrolled after it is on the ESP; prepare, or the PCR 0+7 binding when the
  keys did not go in).
- `tools/test-installer.c`: the Secure Boot rows in setup mode, the
  arguments, the HSI and its failing checks.
- `tools/test-settings-monitors.c`: the rows' order; the Security page
  shows the TPM, Secure Boot and the HSI, with fwupd asked once.
- `tools/test-settings-monitors.c` also feeds the TPM and Secure Boot rows
  with what `locachy-tpm status --json` said in the VM: TPM 1.2 bound,
  cleared and off; TPM 2.0 with own keys; a binding waiting.
- In a VM with swtpm and OVMF (TPM 2.0 with Secure Boot, setup mode first;
  then swtpm as a TPM 1.2): see REGRESSIONS for the runs and their logs.
  - With TPM 1.2, PCR 0 changed under another OVMF build. It also changed
    under the same build once the other one had written the variable store.
    Either way the TPM refused and the passphrase opened the disk.
  - The q35 chipset's TCO watchdog resets the VM about 30 s after a resume
    from hibernation. The resumed kernel's iTCO_wdt does not hold it, and
    nothing in the boot path loads it. The hibernation runs therefore use
    `-global ICH9-LPC.noreboot=true`. This is the VM's chipset: process 1
    does not touch the watchdog, in HEAD or in this tree (HEAD was not run
    through a resume in the VM). It was not seen on the X230.
