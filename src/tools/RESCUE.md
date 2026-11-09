# Rescue mode

[中文](RESCUE.zh.md)

Rescue is a live-medium application independent of Terminal. It has its own
screen, disk and cable state, prompts, output pane and cleanup. It does not
start inside a shell, run the installer, or automatically repair an installation.
The ordinary Terminal remains a separate root shell on the live system.

## Entering and leaving

The installer menu offers **Rescue mode** separately from **Terminal**. The
UEFI and BIOS menus also have **LOCachy Rescue** entries. They retain archiso's
kernel, initramfs and media search options and add `locachy.mode=rescue`.
The rescue entry starts `locachy-rescue.service` directly on tty1 instead of a
getty login or installer. Other boot entries keep the normal installer behavior.

Rescue starts without unlocking disks or enabling remote access. Select an
action with the keyboard or pointer. Status, selectable operation output and
controls occupy separate regions. Tab changes panes; arrows, wheel and
PgUp/PgDn scroll. Copy the selected text with y/Ctrl+C; a copies SSH connection
information and f copies the host-key fingerprint.

The keyboard-layout action selects US, Japanese JIS or a standard console/XKB
layout name using the installed `keytable`. The table lives under `/run` and
applies to Rescue and its optional target terminal, not the installed system's
settings. Recovery passphrases are hidden; showing one is an explicit action.

Exit stops the cable service, unmounts owned filesystems and closes owned
mappings. A busy or changed resource is not forcibly removed: cleanup fails
visibly and Rescue stays open for another attempt. Exiting a target terminal
returns to Rescue. When opened from the installer menu, a successful Exit
returns there. From the dedicated boot entry, it keeps a **Rescue session
closed** page on screen with choices to start a new session, reboot or power
off; reboot and power off require an explicit Y confirmation. It never opens
Terminal or installation automatically.

## Unlocking an existing system

1. Rescan and select the encrypted root by device, size and UUID. Duplicate
   labels or UUIDs are not silently resolved to the first device.
2. Enter the LUKS recovery passphrase locally. `cryptsetup` receives its exact
   bytes through stdin with `--key-file=- --disable-external-tokens --readonly`.
   It does not use a TPM token. The passphrase is not stored in arguments,
   environment variables, logs, an on-disk key file or the ESP.
3. The helper verifies LUKS2 `locachy-crypt`, ext4 `locachy-root`, and the mounted
   installation's identity. Root is mounted `ro,noload`, without journal replay.
   A uniquely associated ESP is mounted read-only at the target's `efi` directory.
   An unsafe, in-use or ambiguous ESP is reported rather than guessed; root can
   still be inspected read-only, but write repair is blocked.
4. The associated encrypted swap is verified separately with the supplied
   passphrase. Rescue never activates it, resumes hibernation or enables
   discard. Two encrypted devices do not imply two separate password prompts.

**Enable repair** is a separate confirmation. Rescue asks for the recovery
passphrase again, closes its read-only resources, verifies the same identities,
and reopens root and the identified ESP read/write. The optional **Target
terminal** runs `arch-chroot` only after this authorization. It does not itself
update packages, rebuild boot files, change keyslots or enroll a TPM. An
incorrect second passphrase leaves the target closed, not silently writable.

The helper journals random session mapper names, DM UUIDs, device numbers and
kernel mount IDs in root-only `/run/locachy-rescue/storage.json`. It does not
adopt an existing mapper or unmount another program's nested mounts. Closing
the target leaves the cable service alone; stopping the cable leaves the target
alone. These are operational safeguards, not restrictions on an already
trusted root administrator.

## Authenticated debug cable

Select **Start debug link**, choose a physical wired port, then set a temporary
session password twice. This is **not** the LUKS passphrase or either system's
root password. It must contain 12–256 bytes, without NUL or line breaks.

The selected port temporarily leaves NetworkManager and enters a dedicated
network namespace. It has only an IPv6 link-local address, no default route,
DNS or forwarding. Other ports are not reconfigured. Rescue saves the port's
identity and NetworkManager state for restoration.

A separate OpenSSH service listens there on port 2222. It uses its own config,
RAM-only Ed25519 host key and private shadow-file view; the live and target
systems' normal root passwords are not changed. Password authentication is
required; forwarding, agent/X11/tunnel access and user rc files are disabled.
The ordinary SSH service remains masked. Remote access grants root rescue
administration, including SSH commands and SFTP transfers; it is not a
read-only security sandbox.

The screen shows the address, port and SHA-256 host-key fingerprint. On the
controlling computer, use its **local cable interface** as the IPv6 scope:

```sh
ssh -S none -p 2222 root@fe80::ADDRESS%LOCAL_INTERFACE
sftp -P 2222 'root@[fe80::ADDRESS%LOCAL_INTERFACE]'
```

Replace the placeholders with the displayed address and the controlling
computer's interface, not the rescue computer's interface name. Verify the
fingerprint against the rescue screen before accepting the host key. A new
session gets a new host key; do not disable host-key verification or blindly
remove an older key. Enter the temporary session password when SSH asks.

**Stop debug link** terminates the service's full process tree, removes its
namespace and credentials, and restores the selected port. The service has a
one-hour maximum lifetime. The UI and cable service are separate: stopping the
link does not exit Rescue or close its target.

## Components and checks

- `progs/rescue.c`: independent screen/input frontend and fixed helper requests.
- `installer/locachy-rescue`: action routing and combined structured status.
- `installer/locachy-rescue-storage`: disk identity, unlock and owned resources.
- `installer/rescue-link.py`: explicit cable session and OpenSSH lifecycle.
- `installer/locachy-rescue*.service`: direct rescue boot and separate cable cgroup.
- `installer/rescue-boot-entries.py`: derives boot entries from packaged templates;
  upstream template changes fail explicitly.

`tools/test-rescue.c` exercises the real UI loop with executable helper doubles:
exact argv/stdin, cancellation, malformed replies, timeout escalation, output,
layout changes and cleanup refusal. The `test-rescue-{dispatch,storage,link}.py`
unit suites exercise backend contracts; `test-rescue-link.py --linux-auth` uses
real OpenSSH in private Linux namespaces. These checks do not replace a real
LUKS/systemd VM or full ISO rescue-session acceptance. No test authorizes
changing a physical installation's disk, TPM, firmware keys or boot state.
