# Touchpads

[简体中文](TOUCHPADS.zh.md)

## Touchpad Clicks

**Settings → Input & Output → Mouse & Touchpad → Touchpad Clicks** chooses
what makes a touchpad click. It is stored as `touchpad_clicks` in
`/etc/locachy/rc.conf`:

| Value | rc.conf | Tap | Pressing the surface | Separate buttons |
| --- | --- | --- | --- | --- |
| tap and press (default) | unset | clicks | clicks | click |
| press only | `press` | nothing | clicks | click |
| pointing only | `pointing` | nothing | nothing | click |

Separate buttons are the TrackPoint's buttons, or a touchpad's own
buttons, like those under the round touchpads of Panasonic's CF-SV and CF-FV
models.

### How it is applied

Direct programs and the shared display both go through libinput, configured
the same way:

- **Tap to click** is libinput's tapping setting. `base/pointer.c` reads
  `touchpad_clicks` when it sets a device up. For the shared display,
  settings writes `<tap>yes</tap>` or `<tap>no</tap>` into the default
  `<libinput><device>` profile of labwc's `rc.xml`. labwc gives each device
  only the one profile that matches it best, so a separate touchpad profile
  would leave the default profile's other lines out for touchpads. labwc
  applies tapping only to devices that can tap, which means touchpads.
- **Pointing only, the press.** A clickpad (kernel `INPUT_PROP_BUTTONPAD`)
  has one button, `BTN_LEFT`, under its surface. libinput has no setting
  that turns it off. Click method `none` only removes the soft button areas,
  and a press anywhere is then a left click. labwc's manual says `none`
  produces no button events, but libinput 1.32 does not behave that way. So
  for each clickpad present, settings writes a section of libinput's local
  quirks file, `/etc/libinput/local-overrides.quirks`, with
  `AttrEventCode=-BTN_LEFT`. libinput quirks cannot match on input
  properties, so the section matches that one device by udev type, bus,
  vendor, product and name. libinput drops the code when it opens the
  device. The touchpad still points, scrolls and does gestures.
- **Pointing only, the click method.** Settings also writes
  `<clickMethod>none</clickMethod>` for labwc, and `pointer.c` sets the same
  method, because the soft button areas are useless once nothing clicks.
  Without them, a finger that starts in the bottom strip points at once. On
  pads at least 50 mm tall, libinput's thumb detection still holds back a
  slow finger there.
- **Separate buttons keep working.** A TrackPoint's buttons belong to the
  TrackPoint's own input device. The kernel routes them there even when
  they are wired through the touchpad (Synaptics pass-through, RMI4 F03,
  Elantech). A touchpad with buttons of its own has `BTN_LEFT` and
  `BTN_RIGHT` and is not a clickpad, so no section matches it. Only tapping
  turns off there.
- **Exception: top soft buttons.** The ThinkPad *40 series
  (`INPUT_PROP_TOPBUTTONPAD`) has no physical TrackPoint buttons. Its
  TrackPoint buttons are areas of the clickpad surface, so they stop
  clicking with pointing only.

The quirks sections stay as long as pointing only stays, including those for
clickpads seen earlier. `settings --apply` runs at boot and after resume,
possibly before a touchpad's driver has probed it, so dropping sections then
would lose them. A clickpad connected for the first time gets its section at
the next `settings --apply` or when settings opens. Choosing another value
removes all of these sections. The rest of the file, including the Touchpad
Circular Scrolling section, is kept.

A quirks file that libinput cannot parse disables every quirk, libinput's
own too. Settings therefore turns any character of a device name that the
parser would not take as it is into `*`.

### When it takes effect

- **Direct programs:** when they next take the pointer (after home). Each
  new libinput context reads rc.conf and the quirks again.
- **Shared display:** tapping and the click method change at once, because
  settings writes labwc's `rc.xml` copy and labwc reconfigures its devices on
  `SIGHUP`. libinput reads quirks only once per context, so the press
  disappears or returns only at the display's next start.

The row's notice says which of these applies to the change just made.

### Tests

- `tools/test-settings-monitors.c` checks the rc.conf key, the labwc lines,
  and the quirks sections: written for clickpads only, kept, and removed.
- `tools/test-touchpad-clicks.sh` runs the real libinput through
  `pointer.c` without devices or root. In a user and mount namespace, tmpfs
  holds the udev and sysfs view of a clickpad, a TrackPoint and a two-button
  touchpad, and their evdev events arrive through pipes.

Both run in `tools/test-interaction-regressions.sh`.
