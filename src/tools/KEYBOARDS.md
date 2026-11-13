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

## Caps Lock on a Japanese layout

XKB's `jp` puts `Eisu_toggle` on the Caps Lock key and `Caps_Lock` on
Shift+that key. LOCachy has no Japanese input method, so `keytable` adds
xkeyboard-config's standard `caps:capslock` option to any layout whose Caps
Lock key types `Eisu_toggle` alone. The key is then Caps Lock, both alone and
with Shift, as on every other layout, and the console keymap (kbd's `jp106`
has `Caps_Lock` there too). The option is written to the key table's `options`
line, so the direct programs, the lock, the boot passphrase, the bar and labwc
all compile the same keymap. In labwc's `environment`, an empty
`XKB_DEFAULT_OPTIONS`, or one with only this option, counts as the table's and
follows it. Any other value counts as configured and stays.

The rule applies only where the plain key is `Eisu_toggle`. A layout whose
options already remap that key (`ctrl:swapcaps`, for example) keeps its own
mapping. To get Eisu back for a Japanese input method, remove that rule from
`progs/keytable.c` (`CAPS_OPTION`). Shift+Caps Lock then locks again, and
plain Caps Lock is `Eisu_toggle`.

The initramfs contains a copy of the tables and device overrides. The boot
passphrase uses new choices only after the next boot-image build. A runtime
layout change does not by itself rebuild or replace that boot image.

Settings in a window on the shared display holds no keyboard (its keys are the
seat's), so it lists the keyboards from sysfs (`/sys/class/input/eventN/device`:
letters and Enter, each id once) and sets them through the same
`keyboards.conf` and reloads as direct settings; nothing in `/dev/input` is
opened (`test-keyboard-devices.c`).

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

English mode (`EN`, Rime's `easy_en`) follows fcitx5's keyboard engine with
word hints: letters make the word, shown with its completions and none
chosen; Tab and Shift+Tab move through them (past either end, none again);
BackSpace edits the word. Any other key (Space, Return, Escape, digits of the
main row or the keypad, punctuation, arrows, Ctrl/Alt/Super combinations)
commits the letters exactly as typed, or the completion Tab moved to, and is
then typed itself. Digits never select in English; Chinese mode keeps its
digit selection. Switching the mode or locking Caps commits the word too.
A window on the shared display (term, view) starts no Rime of its own: the
bar is the input method of every window there. `tools/test-ime-keys.c` covers
the modes with a double of Rime's session; `test-ime.c` and the bar's
`test-bar-keyboard.c` (in `test-input-delivery.sh`) with real Rime.

## Who reads the keyboard

Process 1 turns off the text console's own keyboard handler (`K_OFF`) as it
starts and, once the screen is its own, puts the console in graphics mode
(`KD_GRAPHICS`), as logind and seatd do for a graphical session. A key then
reaches only the program the keyboard is lent to and process 1, which
watches for its own keys; nothing is queued on the console, and the
programs and services process 1 starts have `/dev/null` as their standard
input.

- **Ctrl+Alt+Del** is process 1's, and it asks first: wherever it sees the
  keys (direct programs, home, the boot passphrase, the shared display in
  direct input) it shows "restart the computer now?" on its own screen, with
  the system job a restart would wait for. Y restarts cleanly; N or Esc gives
  the screens back as they were; other keys wait, and Delete held is one
  question. The lock does not act on it. While the input method holds the
  keyboard (CN/EN), the keys go to the bar only, as they did before. While a
  restart or power off already waits for a system job, Ctrl+Alt+Del pressed
  again (a new press, not Delete held) reboots at once, as that screen says
  (`JOBS.md`, "Shutdown").
- **Caps Lock and Num Lock: one state.** With the console's handler off,
  process 1 holds the lock state for every program and shows it on every
  keyboard's LEDs (`EV_LED`, as libinput writes them), a keyboard plugged in
  later included; the locks start off. The LEDs are output only: nothing
  reads them back for the locks, as a keyboard whose LED its firmware drives
  (the X230's embedded controller) may not report what was written
  (`EVIOCGLED`). It sees every lock key a direct program
  sees (their lent keyboards are read-only copies) and gives the state with
  the keyboards (`take LOCKS`, `keys LOCKS`); a program starts from it, never
  from one keyboard's LEDs. On the shared display the bar reports each change
  on the seat's link (`locks LOCKS`, passed on to process 1), since in CN/EN
  mode it holds the keyboards out of process 1's sight; it starts from the
  state given with the seat and hands it to labwc with its virtual keyboard's
  modifiers. labwc-locachy's `keyboard-locks.patch` makes a new keyboard join
  with the seat's locks, takes a host virtual keyboard's locks as the seat's,
  and drops them when its session is disabled. Process 1 shows its state
  again each time it places the keyboards. The full-screen installer sets the
  LEDs too.
- **The bar** reads only the keyboards process 1 lends with the seat; seat
  passes them on. When home or another direct program comes to the front,
  process 1 revokes them (`EVIOCREVOKE`, which also ends a grab), and seat
  revokes labwc's input devices before it disables labwc's seat, as seatd
  does. A lent keyboard still types in its own layout: the bar looks it up in
  `keyboards.conf` by its id.
- **A full input queue** (`SYN_DROPPED`): the rest of that frame is dropped and
  the keys are read again from the kernel (`EVIOCGKEY`), so a lost release
  leaves no modifier or key held and nothing is typed.

`test-keyboard-devices.c` covers the LEDs, the read-only lent keyboards and
`SYN_DROPPED`; `test-keyboard-locks.c` one lock state across process 1, a
direct program and the shared display (two keyboards at boot, a grabbed
desktop's report, hotplug); `test-bar-keyboard.c` the bar's lent keyboards,
draining, revocation and its locks; `test-keytable.c` the layout of a keyboard the bar reads;
`test-input-hotplug.c` hotplug with the shared display in front.
The real console, revocation in a VM and the X230's Caps Lock LED still need
their acceptance runs.

## Keys and actions

Programs do not test raw keys for the shared keys. `base/key-action.c` maps a key and its modifiers to one action (`key_action`; `key_list_action` for a list where nothing is typed, where q is Esc), and programs react to the action. The keys arrive like this: a Ctrl letter as its control code (Ctrl+Q is 17, Ctrl+C is 3, Ctrl+Shift+letter the same code with Shift); Ctrl with a digit as `K_CTRL_0` plus the digit; the function keys as `K_F1` plus n; Alt and Super letters as the plain letter with the modifier. `test-key-actions` pins the table, and `test-key-literals.py` fails on a program that tests a raw key for these actions.

| Action | Key |
| --- | --- |
| Quit (asks first when work would be lost) | Ctrl+Q; the window's close and Alt+F4 are the same request |
| Cancel the innermost menu, dialog, field, search or selection; else back one level; at the top, nothing | Esc |
| Home: freezes the program, keeps its state, does not quit | Super+Esc (also Ctrl+Alt+Esc, Ctrl+Alt+Super, ThinkVantage) |
| Help: this program's keys | F1 |
| Next and previous field | Tab, Shift+Tab |
| Copy, cut, paste | Ctrl+C, Ctrl+X, Ctrl+V (Ctrl+Insert, Shift+Insert) |
| Undo, redo | Ctrl+Z, Ctrl+Shift+Z |
| Select all; deselect | Ctrl+A; Ctrl+Shift+A (in a log or a pane, Ctrl+A then Ctrl+C copies all of it) |
| Find; find in files; next and previous match | Ctrl+F; Ctrl+Shift+F; F3, Shift+F3 |
| New, open, save, save as | Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S |
| New folder | Ctrl+Shift+N |
| New tab, reopen tab, close tab | Ctrl+T, Ctrl+Shift+T, Ctrl+W |
| Next and previous tab | Ctrl+PgDn, Ctrl+PgUp (Ctrl+Tab, Ctrl+Shift+Tab) |
| Back, forward, parent | Alt+Left, Alt+Right, Alt+Up |
| Rename; reload | F2; F5 |
| Delete to the trash; delete for good | Delete; Shift+Delete |
| Properties | Alt+Enter |
| Hidden files | Alt+. |
| Zoom in, out, actual size | Ctrl+= (Ctrl++), Ctrl+-, Ctrl+0 |
| Context menu | Shift+F10 or the Menu key |

Some keys cannot be bound on their own: Ctrl+H, Ctrl+I, Ctrl+J, Ctrl+M and Ctrl+[ are the codes of Backspace, Tab, Enter and Esc. A shared action has only its key from the table: copy is Ctrl+C, never a letter (no y, c, x, v or u). A program's own single-letter command in a list is the first letter of its action (R start, K stop, L logs); a letter typed into a field is text.

Each program's own keys (edit's folding on Ctrl+Shift+[ and ], view's Ctrl+G to a page, files' Ctrl+Alt+C to copy a path, and the rest) are listed by its F1 help, above the keys every program has.

A text field takes its editing keys before the table: Ctrl+W deletes the word before the cursor (a field has no tab to close), Ctrl+U deletes to the start and Ctrl+K to the end; Home and End go to the start and the end (Ctrl+A selects all).

Where the table does not apply: the terminal hands every key to its shell except home and the window's close; the extended display's session hands every key to the Mac, and nothing leaves it while the link is up (EXTDISPLAY.md); the installer cannot be left while it installs (on its steps Ctrl+Q asks first, since the choices made would be lost); the lock neither quits nor goes home.
