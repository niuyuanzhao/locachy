# LOCachy idle screen power and display ownership

[中文](DISPLAY-POWER.zh.md)

## Independent controls

- **Security & Privacy → Screen Locking → Auto-lock** controls idle locking and the lock before lid sleep or critical-battery hibernation. Its saved idle delay is `lock_after`, ten minutes by default; `0` disables only idle-triggered locking. After an idle lock, **Unlock without password within** (`lock_grace`, 5 seconds by default; `0` asks immediately) lets input unlock without authentication. It does not apply to the lock key, lid, sleep, critical-battery or high-threat lock. The delays remain configured when Auto-lock is off. High threat refuses the off setting and disables the grace.
- **Power & Battery → Display → Automatic display-off** controls only blanking the displays. `auto_blank="1"` enables it; missing or `0` means off. `blank_after` is the independently stored delay in minutes, from 1 to 60 (default 5). Invalid/missing values use the default, not zero. Changing or disabling the timer does not discard the stored delay.
- The two idle deadlines are independent. Input wakes the display but does not authenticate or unlock a locked session. Display-off does not suspend the machine or alter brightness, lid behaviour, or a manual lock request. Existing Wayland idle inhibitors keep both shared-desktop timers off while their work continues.

## Output ownership

Process 1 uses the connectors' DRM DPMS property only while it owns DRM and only restores outputs it powered down. An active extended-display task is explicitly excluded: the user may be entering a pairing code, and a paired session holds the existing live-link lock deferral. No output lease is changed and the sender connection is not stopped, restarted, or awakened as a side effect.

While labwc owns DRM, its trusted `labwc-control` uses the compositor's output-power protocol. The helper records only outputs whose power it changed, restores those on pointer/keyboard activity or seat release, and does not change saved monitor modes or positions. A lost power controller returns control to process 1 rather than leaving a knowingly black session. The output power protocol is not exposed to sandboxed containers.

## Power profile and zram

Both are rc.conf keys, written by make.sh only in an old rc.conf that lacks them:

- `power_profile` (**Settings: Power & Battery**): `auto` (battery power saving,
  AC balanced; the default), `power`, `balanced` or `performance`. It is meant to
  set the CPU energy preference and the ACPI platform profile.
- `zram`: `"1"` (the default) keeps compressed swap in RAM on `/dev/zram0`, above
  the disk swap, when the machine has at least 512 MiB of RAM; `"0"` turns it
  off. It is turned off before hibernation, so the hibernation image goes to the
  disk swap (`teardown_zram_swap` in `init/init.c`).

Status: `init/init.c` applies `power_profile` (`apply_power_profile`). It writes
the CPU `energy_performance_preference` (intel_pstate or amd_pstate) and the ACPI
`platform_profile` when the setting changes and on an AC/battery change, and
re-applies it on SIGHUP. Not verified on hardware yet.

## Kernel and driver defaults

make.sh installs these LOCachy defaults. Each is replaced by a file of the same
name in the directory that takes precedence over it.

- **Audio:** `/usr/local/lib/modprobe.d/50-locachy-pm.conf` (from
  `link/50-locachy-pm.conf`): `options snd_hda_intel power_save=1
  power_save_controller=Y`. An HDA codec powers down after 1 s of silence. If a
  codec pops when it wakes, put `power_save=0` in
  `/etc/modprobe.d/50-locachy-pm.conf`, which masks this file. Wi-Fi power saving
  is NetworkManager's, not a module option.
- **PCI and USB runtime PM, SATA link power:**
  `/usr/local/lib/udev/rules.d/85-locachy-pm.rules` (from
  `link/85-locachy-pm.rules`):
  - PCI devices get `power/control=auto`, except display controllers (class
    `0x03`) and communication controllers (`0x07`, such as Intel MEI and serial
    ports), which keep their driver's policy.
  - USB devices get `auto`, except HID (`03`) and wireless (`e0`) devices. A
    device with an interface of class HID (`03`), audio (`01`), video (`0e`),
    wireless (`e0`), CDC (`02`, `0a`) or printer (`07`) keeps its parent on
    (`power/control=on`). The interface rules also run on `bind`, because udev
    does not order the events of different devices.
  - SATA hosts get `link_power_management_policy=med_power`; the DIPM variant is
    left out, as it causes I/O stalls on some SSDs.
  - I2C devices are left alone: touch controllers and pens sit there.
  - To change one rule, put a file of the same name in `/etc/udev/rules.d`; to
    disable the whole file, link it to `/dev/null` there.
  - Check on the machine after install: `grep -r . /sys/bus/usb/devices/*/power/control`.
- **Wi-Fi:** `/etc/NetworkManager/conf.d/30-locachy-network.conf` (from
  `link/NetworkManager.conf`) sets `wifi.powersave=3`, which enables Wi-Fi power
  saving.
- **Writeback and swap read-ahead:** `/usr/local/lib/sysctl.d/50-locachy.conf`
  (from `link/50-locachy-sysctl.conf`): `vm.dirty_writeback_centisecs = 1500`
  (write back every 15 s instead of 5 s, for fewer disk wake-ups) and
  `vm.page-cluster = 0` (no swap read-ahead, which only costs time on zram). The
  NMI watchdog stays on: it is what reports hard lockups.

## Check

`tools/test-init-screen.c` covers independent deadlines, failure without a timer spin, waking the direct path, preserving an active extdisplay session, compositor-owned output, and a late auto-lock request when Auto-lock is off. `tools/test-screen-power.sh` covers restoring only the lessor's display-power changes without changing leases or waking an output that was already off. The labwc test path covers private seat messages and supported output-power protocol behavior; a device-free pass does not replace physical-panel acceptance.
