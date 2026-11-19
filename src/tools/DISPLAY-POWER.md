# LOCachy idle screen power and display ownership

[中文](DISPLAY-POWER.zh.md)

## Independent controls

- **Security → Screen Locking → Auto-lock** controls idle locking and the lock before lid sleep or critical-battery hibernation. Its saved idle delay is `lock_after`, ten minutes by default; `0` disables only idle-triggered locking. After an idle lock, **Unlock without password within** (`lock_grace`, 5 seconds by default; `0` asks immediately) lets input unlock without authentication. It does not apply to the lock key, lid, sleep, critical-battery or high-threat lock. The delays remain configured when Auto-lock is off. High threat refuses the off setting and disables the grace.
- **Power & Battery → Display → Automatic display-off** controls only blanking the displays. `auto_blank="1"` enables it; missing or `0` means off. `blank_after` is the independently stored delay in minutes, from 1 to 60 (default 5). Invalid/missing values use the default, not zero. Changing or disabling the timer does not discard the stored delay.
- The two idle deadlines are independent. Input wakes the display but does not authenticate or unlock a locked session. Display-off does not suspend the machine or alter brightness, lid behaviour, or a manual lock request. Existing Wayland idle inhibitors keep both shared-desktop timers off while their work continues.

## Output ownership

Process 1 uses the connectors' DRM DPMS property only while it owns DRM and only restores outputs it powered down. An active extended-display task is explicitly excluded: the user may be entering a pairing code, and a paired session holds the existing live-link lock deferral. No output lease is changed and the sender connection is not stopped, restarted, or awakened as a side effect.

While labwc owns DRM, its trusted `labwc-control` uses the compositor's output-power protocol. The helper records only outputs whose power it changed, restores those on pointer/keyboard activity or seat release, and does not change saved monitor modes or positions. A lost power controller returns control to process 1 rather than leaving a knowingly black session. The output power protocol is not exposed to sandboxed containers.

## Check

`tools/test-init-screen.c` covers independent deadlines, failure without a timer spin, waking the direct path, preserving an active extdisplay session, compositor-owned output, and a late auto-lock request when Auto-lock is off. `tools/test-screen-power.sh` covers restoring only the lessor's display-power changes without changing leases or waking an output that was already off. The labwc test path covers private seat messages and supported output-power protocol behavior; a device-free pass does not replace physical-panel acceptance.
