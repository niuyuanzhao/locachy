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
Shift/Lock/Control/Alt/Super modifiers and release events, as mature Rime
frontends do. Num Lock affects the resolved keypad symbol (`KP_1` versus
`KP_End`); its modifier bit is not passed as an unrelated Rime mode switch.
JIS Shift+Eisu is recognized from the actual XKB Caps Lock transition, not an
assumed unshifted physical key role. Lock state is shared across the keyboards.

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
