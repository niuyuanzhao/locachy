# Display-set layouts

[中文](MONITORS.zh.md)

Settings saves the connected display set as one layout in
`/etc/locachy/monitor-profiles.conf`. Each set keeps its resolution and exact
millihertz, scale, relative placement and optional signed logical X,Y. A
laptop alone, that laptop with a desk monitor, and an external-only session
are independent profiles. Saving one set replaces only that set.

The identity is a hash of the kernel-provided raw EDID bytes, not an EDID
parser. A unique display can move between connectors. Identical EDIDs in
the same set are disambiguated by connector; without EDID the connector is
the identity. Thus indistinguishable monitors swapped between ports cannot
be identified reliably. Hotplug identity discovery uses the same host
sysfs path in settings, the controller and direct KMS.

The old `monitors.conf` remains a read-only legacy fallback. An unmatched
set uses it in memory; it does not overwrite any saved profile. Settings
shows whether a saved set matched or fallback is being used. Connecting the
rest of a remembered set selects its full profile. The controller rechecks
on head addition/removal, late enable, or advertised-mode changes, without
idle polling. File updates also reselect while the display is active.

Profile records are `profile=HASH`, one tab-separated
`IDENTITY CONNECTOR PLACE,WIDTHxHEIGHT@MILLIHZ,SCALE[,X,Y]` line per output,
then `end`. Files are bounded when rewritten, written beside the old file,
flushed, synced and renamed. They are local configuration, not source files.
The legacy mode without `@MILLIHZ` is still accepted and does not force the
highest refresh rate. The legacy `preferred` value means automatic:
keep an advertised active mode first, otherwise use the preferred mode.
Settings obtains available timings from DRM; the
resolution row shows Hz with each advertised mode. No custom modeline or
bandwidth estimate is generated. For example `1920x1080@59940` is
59.940 Hz. Direct KMS retries a refused timing with the previous active
advertised timing, then advertised timings with lower pixel-clock demand;
it logs that temporary fallback without modifying the saved target.

The controller tests an entire configuration before applying it. A refused
test/commit leaves the active compositor layout, logs the refusal, and does
not repeatedly submit the same failed target. A topology change invalidates
a pending test and causes fresh selection. A saved mode absent from the
current advertised list is reported; the current usable mode is retained
rather than silently replacing the saved desire with another rate.

`tools/test-monitor-profiles.sh NEW_ABSOLUTE_DIRECTORY` checks independent
profiles, fallback preservation, identity/port changes, duplicate identities,
atomic-save failure, exact mHz round trips, late-enable events and controller
transaction failure/cancellation. The profile tests also run with ASan and
UBSan. Real link-bandwidth and monitor hotplug acceptance remains separate.
