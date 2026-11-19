# Disk encryption, the TPM and Secure Boot

[中文](SECURITY.zh.md)

This file covers the encrypted disk, how the TPM opens it, Secure Boot with
the machine's own keys, hibernation, and fwupd's host security report.

## What there is

**The disk.** The installer makes a LUKS2 root (`locachy-crypt`) and a
LUKS2 swap (`locachy-swap-crypt`), both opened by the same long passphrase.
Enrollment preserves the recovery-passphrase slot. When TPM unlock fails, one typed
passphrase is tried on both root and swap, not one separate prompt per disk.

There is no PIN anywhere; the passphrase is the only typed secret.

**TPM 1.2** (ThinkPad X230): `locachy-tpm enroll` seals a random key to PCR
0 with `TPM_Seal` and stores the blob in a `locachy-tpm12` LUKS2 token.

**TPM 2.0, without this machine's own Secure Boot keys**:
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
TPM-only resume still asks for nothing at boot and preserves the session's lock state. Failed or
forged root tokens fall back to the passphrase, and failed passphrase attempts
never release a root key from the hibernation image. Each start's `swapon` and each
shutdown discard the whole swap (`SWAP_FLAG_DISCARD`, `DISCARD_ONCE`;
`cryptsetup --allow-discards`). A resumed image does not stay in the swap.

**fwupd's Host Security ID.** It is the output of `fwupdmgr security`. The
installer shows the HSI and the checks that failed at the encryption step,
which needs fwupd on the live medium (`installer/packages.x86_64`).
Settings shows the HSI under Security, Host Security. Enter there runs
fwupd's whole report in a terminal, with the fixes fwupd offers.

## Idle lock and display power

In **Settings → Security → Screen Locking**, **Auto-lock** controls idle locking
and locking before lid sleep or critical-battery hibernation. Its independent
idle time (`lock_after`, 10 minutes by default) is kept when the switch is off.
A saved time of `0` means never lock for idle alone. After an idle lock, the
separate **Unlock without password within** grace (`lock_grace`, 5 seconds by
default; `0` asks immediately) lets a key or pointer input unlock without the
password. It does not apply to the lock key, lid, sleep, critical-battery lock,
or a high-threat lock. Both manual locking and the separate boot lock still
work. The high threat level keeps Auto-lock enabled and disables the grace.

In **Settings → Power → Display**, **Automatic display-off** and its idle time
(`auto_blank` and `blank_after`, off and 5 minutes by default) affect the
outputs only. Input lights them without dismissing an existing lock. Either
idle action can fire first; turning one off does not turn the other off. An
active Wayland idle inhibitor (for example a video) holds off both timers on
the shared desktop. While Extended Display is on screen, automatic display-off
leaves its output alone, and a live link keeps its existing idle-lock deferral.
The feature does not change the lid sleep action, brightness or the extended
display connection.

With Auto-lock off, an unlocked session can resume from hibernation unlocked;
this does not change the checks that authorize access to the encrypted disks.

## Threat model

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
| An older signed image (downgrade) | **Not stopped.** Every image ever signed with this PCR key and db key opens the disk; `locachy-prev.efi` may be one of them on the ESP, but an old unsigned Previous is not re-signed when keys are first created. A signed policy has no revocation. This matters if an older image has a flaw before `leave-initrd`. Remedy, not automated: a new PCR key (`ukify genkey`, then `locachy-tpm enroll`) invalidates all older images. | n/a | n/a |
| Root on the running system | Cannot unseal after `leave-initrd`. It can sign images with the PCR and db private keys on the encrypted root (`/etc/systemd`, `/var/lib/sbctl`), so a compromised root can leave a signed backdoor image. | Can unseal at any time; it already has the disk. | Can unseal. |
| Stolen swap (disk out) | Stopped, as the root. | Stopped. | Stopped. |
| Hibernated machine taken | The swap opens with nothing typed. With Auto-lock on, the session comes back at the lock screen; an unlocked image made with Auto-lock off resumes unlocked. `leave-initrd` is measured first, so the restored system cannot unseal. The kernel runs with `intel_iommu=on` against DMA. | As own keys, without the `leave-initrd` effect. | As PCR 0+7. |
| Swap bound to the TPM, root to its passphrase only | Stopped: process 1 requires an actual successful root unlock before any encrypted resume. Merely adding unauthenticated TPM-token JSON to the root header cannot bypass the passphrase. | Same. | Same. |
| Stale hibernation image or swapped-out pages | Discarded at each start and each shutdown on disks with discard. **Not removed** on disks without discard (hard disks), where they stay encrypted with the swap's key. The SSD may keep physical copies after TRIM. Discards through dm-crypt show which blocks are used (the cryptsetup FAQ's trade-off), for the swap only. | Same. | Same. |
| TPM bus sniffing (discrete TPM) | Passive sniffing: stopped. systemd salts the unseal session with the SRK recorded at enrollment, and the secret crosses the bus encrypted. **Not stopped**: an active interposer (TPM reset and replayed PCR values, a fake TPM). | Same. | **Not stopped**: TPM 1.2 returns the key in the clear over LPC. |
| Firmware rewritten (no Boot Guard on the X230) | **Not stopped**: the firmware is the root of trust and can fake measurements. | **Not stopped.** | **Not stopped.** |
| Microsoft keys enrolled (`--microsoft`) | The disk stays closed to Microsoft-signed systems (PCR 7, above). sbctl enrolls no dbx, so revoked Microsoft binaries still **start**. That breaks only the claim that LOCachy alone starts, not the disk. | n/a | n/a |
| Recovery path | Enrollment preserves the passphrase slot. It is asked whenever the TPM refuses, with what happened shown in red. The waiting binding's slot uses a 256-bit random key kept inside the encrypted root, so it adds no way in from outside. It is removed once the binding is done, by `enroll`, or by `clear`. | Same. | Same. |
| No lock at boot with TPM unlock | Refused together, in the installer and in settings. | Same. | Same. |

Enrollment safeguards:

- A TPM 2.0 binding made without own keys passes `--tpm2-public-key=` so
  `systemd-cryptenroll` never implicitly attaches a signed PCR 11 policy from an
  existing PCR key.
- Key enrollment refuses to proceed when no signed image is present on the ESP.
- Re-binding removes the old TPM token only after the new enrollment succeeds.
- Pending-key checks and typed-passphrase unlock use cryptsetup's
  `--disable-external-tokens`: an existing usable TPM token must not make an
  unrelated key appear valid. Token-only boot unlock remains separate.
- Previous is copied unchanged from the old current UKI, not re-signed when
  keys are created. An unsigned old image cannot boot with Secure Boot on.
- When a hibernation image does not resume, the TPM token is tried before
  falling back to the passphrase.

Known limitations:

- Downgrade to older signed images (above).
- Hibernation on kernels whose lockdown blocks it under Secure Boot. Arch
  and CachyOS kernels do not lock down by themselves; the VM shows what this
  kernel does.
- Discard-less disks keep stale swap contents, encrypted.

## Pending-binding failure recovery

The implementation saves the key and UUID association atomically before changing
the LUKS header, retains failed devices across missing disks and failed slot
removals, records completed enrollment separately from cleanup, and retries slot
retirement safely while keeping the long recovery passphrase intact.

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
    The helper suite has 18 tests, including external-token false positives
    and root/swap transition-slot verification.
- `tools/test-init-unlock.py` compiles the actual init unlock/resume functions
  with block, TPM and UI doubles, and runs the actual cryptsetup fork/pipe/exec
  path against a child executable. Fifteen checks cover root authorization,
  token/passphrase fallback, duress precedence, high threat level, exact
  root/swap passphrase bytes and arguments, and token-only null stdin.
- `tools/test-install.py`: the installer's advice, `--secure-boot`
  checks, fwupd's report, and the order (keys made before the image,
  enrolled after it is on the ESP; prepare, or the PCR 0+7 binding when the
  keys did not go in).
- `tools/test-installer.c`: the Secure Boot rows in setup mode, the
  arguments, the HSI and its failing checks.
- `tools/test-settings-monitors.c`: the rows' order; the Security page
  shows the TPM, Secure Boot and the HSI, with fwupd asked once.
