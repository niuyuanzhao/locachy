# Adaptive Battery Charging

[中文](BATTERY-CHARGING.zh.md)

Adaptive Charging under **Settings → Power & Battery** is off by default. The
manual start and stop thresholds remain available. On supported hardware,
adaptive mode uses local battery history to recognize prolonged use on AC,
hold near 80% rather than keeping the battery full, and allow charging to
100% before a reliably observed unplug time. It does not deliberately drain a
battery that is already above the target. Without sufficient observations it
leaves normal and configured manual charging in effect.

**Charge to Full Now** temporarily requests 100% while connected to AC. It
changes neither the manual settings nor the learned history. The request ends
when unplugged or at its displayed deadline (at most 24 hours). The driver may
still delay charging because of its firmware policy or temperature limits.

The existing `battery-history` service manages the limits and records their
readback, not just the requested values. Sleep, shutdown, and missing AC samples
are not evidence that the machine remained plugged in. The service does not wake
a sleeping machine just to change the charge target. Turning adaptive mode off
restores manual limits or the thresholds that were in force before adaptive
charging took control. Adaptive control requires one inserted system battery
with a readable pack identity and the standard Linux charge-end threshold.
It does not assume that multiple packs have independent controllers. Battery
readings, history, and existing manual controls remain available where supported.
Older history without verified AC data is displayed but does not train the policy.

## Configuration

- `battery_charge_adaptive="0"`: set to `"1"` to opt in on supported hardware.
- `battery_charge_start` and `battery_charge_end`: saved manual limits, in percent.
- `battery_charge_full_until`: temporary deadline written by **Charge to Full Now**,
  not a setting to keep as a permanent charge limit.

Software tests exercise decisions against timestamped history and the threshold
writes, readbacks and restore path using simulated power-supply devices. They do
not establish that a specific laptop firmware stops charging at the target.
