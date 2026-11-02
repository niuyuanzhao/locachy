# Keyboard layouts

[简体中文](KEYBOARDS.zh.md)

In **Settings → Input & Output → Keyboard**, **System keyboard layout** sets
what a keyboard types unless it has an explicit layout below that row. These
are physical key layouts, separate from the optional Chinese input method.

For a US default with a built-in Japanese keyboard:

1. Give the built-in keyboard its explicit Japanese layout (`jp106`, for
   example).
2. Set **System keyboard layout** to `us`.
3. Leave unconfigured keyboards on **The system's**. A newly connected one
   then uses US unless it has its own saved layout.

Choosing a layout explicitly keeps that choice even when it currently equals
the system default. **Delete**, or choosing **The system's**, removes the
per-keyboard override. Device overrides are stored in
`/etc/locachy/keyboards.conf`; the default is `keymap` in
`/etc/locachy/rc.conf`.

The system selector generates a replacement key table before applying it.
A generation, configuration-write or file-replacement failure leaves the old
layout active. Successful changes reload direct input and the shared display
without rebuilding the system. labwc reads its standard `environment` file
on reconfiguration: layout, model and variant come from the generated table;
unrelated environment entries and explicitly configured XKB options stay.
`/etc/vconsole.conf` receives the same default while keeping other settings.

The initramfs contains a copy of the tables and device overrides. The boot
passphrase uses new choices only after the next boot-image build. A runtime
layout change does not by itself rebuild or replace that boot image.

`test-settings-monitors.c` covers the default/override distinction, US default
with explicit JIS and inherited USB input, picker confirmation/cancellation,
staged generation/configuration/replacement failures, and preserved labwc
options. Runtime keyboard handover and focused-client delivery need their
separate input tests; this UI test does not prove that handover is safe.

## Lock keys and the input method

The shared display sends Rime the layout's native XKB keysym, standard
Shift/Control/Alt/Super modifiers and release events. Mode selection
(`RAW` / `CN` / `EN`) and Caps Lock passthrough stay in the host front end,
and Rime's internal `ascii_mode` is kept off. Num Lock affects the resolved
keypad symbol (`KP_1` versus `KP_End`); its modifier bit is not passed as an
unrelated Rime mode switch. JIS Shift+Eisu is recognized from the actual XKB
Caps Lock transition, not an assumed unshifted physical key role. Lock state is
shared across the keyboards.

Direct-input key tables now contain sixteen Shift/Caps/AltGr/Num combinations.
Old eight-state tables remain readable, but cannot supply the Num-dependent
symbols they never recorded. The normal LOCachy rebuild regenerates those
tables; no manual package or keyboard-configuration edit is required.

`tools/test-input-delivery.sh` tests real Rime with a headless focused client:
uppercase text from JIS and US, restoring ordinary composition after Caps off,
keypad digits/navigation with Num on/off, and unchanged idle English
punctuation in RAW/CN/EN. `tools/test-caps-ime.c` uses generated JIS/US tables and
real Rime for the direct path. These do not validate physical keyboard LEDs
or replace hardware acceptance.

Reference: [fcitx5-rime](https://github.com/fcitx/fcitx5-rime), its Rime
key-event/modifier adapter.

## Who reads the keyboard

Process 1 turns off the text console's own keyboard handler (`K_OFF`) as it
starts and, once the screen is its own, puts the console in graphics mode
(`KD_GRAPHICS`), as logind and seatd do for a graphical session. A key then
reaches only the program the keyboard is lent to and process 1, which
watches for its own keys; nothing is queued on the console, and the
programs and services process 1 starts have `/dev/null` as their standard
input.

- **Ctrl+Alt+Del** is process 1's: a clean reboot wherever it sees the keys
  (direct programs, home, the lock, the boot passphrase, the shared display in
  direct input). While the input method holds the keyboard (CN/EN), the keys
  go to the bar only, as they did before.
- **Caps Lock and Num Lock LEDs.** With the console's handler off, process 1
  sets the LEDs for the direct programs (`EV_LED`, as libinput writes them);
  their lent keyboards are read-only, and a program takes the lock state from
  the LEDs each time it is given the keyboard. The shared display's labwc
  sets its own. The full-screen installer sets them too.
- **The bar** reads only the keyboards process 1 lends with the seat; seat
  passes them on. When home or another direct program comes to the front,
  process 1 revokes them (`EVIOCREVOKE`, which also ends a grab), and seat
  revokes labwc's input devices before it disables labwc's seat, as seatd
  does.
- **A full input queue** (`SYN_DROPPED`): the rest of that frame is dropped and
  the keys are read again from the kernel (`EVIOCGKEY`), so a lost release
  leaves no modifier or key held and nothing is typed.

`test-keyboard-devices.c` covers the LEDs, the read-only lent keyboards and
`SYN_DROPPED`; `test-bar-keyboard.c` the bar's lent keyboards, draining and
revocation; `test-input-hotplug.c` hotplug with the shared display in front.
The real console, revocation in a VM and the X230's Caps Lock LED still need
their acceptance runs.
