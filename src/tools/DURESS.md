# Lock mode and the duress password

[中文](DURESS.zh.md)

Two separate settings, both in Security:

- **Lock mode** (`lock_mode` in rc.conf: `normal`, or `high`) decides *which ways
  to unlock exist*. Normal: the password, plus a fingerprint or a face when each is
  switched on. High: the password only, everywhere it is asked:
  - at power-on, the LUKS passphrase only (the TPM never opens the disk);
  - at the lock screen, the password only;
  - for `sudo`, the password only.
  Switching to high removes the TPM's key slots (`locachy-tpm clear`), and settings
  does not switch while one is left. The TPM token is sealed to every boot image this
  machine's PCR key signs, the previous one and any older copy from the ESP included,
  not to the current image alone; a slot left in place would let such an image open
  the disk. Switching back to normal restores the fingerprint and face switches as
  they were; the TPM has to be bound again. High mode is changed in settings; the boot
  image is rebuilt, so the change takes effect at the next start.

- **Duress** (`duress_enabled` in rc.conf, and a verifier in `/etc/locachy/duress`)
  decides *what a second password does*. Typing the duress password where a password
  is asked (power-on passphrase prompt, lock screen, `sudo`) destroys the encrypted
  disk at once and powers the machine off. It is a different decision from the mode:
  a machine can run normal mode with duress armed, or high mode without it.

## What duress destroys

`duress_wipe` (base/duress.c) overwrites the first part of the two LOCachy LUKS2
partitions (labels `locachy-crypt` and `locachy-swap-crypt`) up to their data offset,
twice, the first pass random. That contains both binary headers, the JSON areas and
every key slot: the passphrase slots, the TPM's sealed key and any recovery slot.
Without them the data cannot be decrypted by anyone, the passphrase included. It
also removes the keys that sign boot images and the duress verifier itself, then
powers off.

It does **not** clear the TPM. The sealed blobs live in the LUKS keyslots, which are
gone; a TPM clear would also break other systems' sealed keys on the same machine
(a Windows BitLocker install, for one).

It does nothing on a system without the LOCachy LUKS2 partitions. Settings and
`locachy-duress set` refuse to arm it there: a wipe that deletes files is neither
quick nor final.

## Precedence: the real credential always wins

- **Power-on:** the passphrase is tried first. The duress check runs only when the
  passphrase opened neither partition.
- **Lock screen and `sudo`:** the typed text is checked against the login password in
  `/etc/shadow` first (`duress_login_check`). Only a *known* mismatch (result 0) can
  wipe. An unreadable shadow entry (result -1) never wipes. This covers the case where
  the right password is refused by `pam_faillock`'s lock.
- `locachy-duress set` refuses a duress password equal to the login password, or one
  that opens the disk (`cryptsetup open --test-passphrase`). Such a password would
  never wipe, so the refusal is the honest answer.

## Setting it up

1. Settings → Security → **Duress password** → Enter. Type it twice. It must be at
   least eight characters, and different from the login password and the disk
   passphrase.
2. Settings → Security → **Duress wipes the disk** → on. This adds
   `auth optional pam_locachy_duress.so` to `/etc/pam.d/sudo`, before its first auth
   line (after `pam_locachy_any.so`'s, when a fingerprint or a face is on), and
   rebuilds the boot image, which then carries the verifier. The line has to come
   first: Arch's `system-auth` ends the stack on a wrong password
   (`pam_faillock.so authfail`, `[default=die]`), so a module after it would never see
   the duress password. It reads the password once (`pam_get_authtok`) and leaves it
   for `pam_unix` (`try_first_pass`), so sudo still asks only once.
3. Optional: **Lock mode** → high.

To undo: Delete twice on the duress password row (it clears the verifier and turns
duress off), or `locachy-duress clear`.

## Limits, stated plainly

- **The verifier is on the unencrypted ESP.** The initramfs (which is in the signed
  UKI, on the FAT ESP) carries it so that the power-on prompt can check it. Anyone
  with the disk can try guesses offline, at PBKDF2-HMAC-SHA256 with 600,000 rounds.
  The duress password is only as strong as its own length: choose it as carefully as
  the passphrase.
- **`sudo` is included** because GrapheneOS covers every credential prompt. A mistyped
  duress password at a `sudo` prompt is a wipe. If that is too sharp, remove the line
  from `/etc/pam.d/sudo`; the power-on and lock-screen checks stay.
- **The previous boot image keeps the policy it was made with.** The firmware's
  "LOCachy (Previous)" entry starts the image from before the last rebuild: after
  duress is armed, its password changed or the mode switched, that image keeps the
  old policy until the next rebuild, and its power-on prompt checks no duress
  password, or the old one. It opens nothing the current one would not: the
  passphrase still has to be typed, and in high mode no TPM key slot is left.
- The wipe is irreversible by design. Nothing in LOCachy keeps a copy of the headers.
- The lock screen wipes from a root process; the power-off uses the kernel's reboot
  call, which needs the capability that root has.

## What is tested

`tools/test-duress.sh` builds `base/duress.c` against scratch files only (verifier
format and permissions, damaged and weakened verifiers, the no-LUKS no-op, and the
wipe's exact extent on fake LUKS2 images, with the other partitions untouched), and
builds it for process 1 (musl, static). The wipe itself is tested with the power-off
disabled, so no test touches a real disk, key or TPM.

`tools/test-init-unlock.py` runs process 1's own power-on unlock functions with the
disks, the TPM and the duress verifier doubled. Armed, the duress password wipes only
after the passphrase was tried with it; the real passphrase, or any text that opens
the disk, never wipes. Unarmed, the duress password is a wrong passphrase. In high
mode no TPM token is tried, even one that would open the disk.

In a virtual machine (QEMU with OVMF, a LOCachy installation on LUKS2, each case on a
fresh copy of its disk), in normal and in high lock mode:
- power-on: a wrong passphrase asks again and changes nothing on the disk. The
  duress password powers off: both LUKS2 headers (the primary and the secondary
  copy) and the key slots of both partitions are overwritten, the ESP's boot sector
  stays, and the next start reaches no system;
- the lock screen and `sudo`: a wrong password changes nothing and the login password
  unlocks; the duress password wipes as above;
- duress turned off (verifier removed, boot image rebuilt): the same password at
  power-on is only a wrong passphrase.

That machine had no TPM: high mode's skipping of the TPM is tested by the unit test
alone. Not yet tested on real hardware.
