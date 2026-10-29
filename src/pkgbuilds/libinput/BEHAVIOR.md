# LOCachy libinput behavioral release gate

`test-libinput.sh` is intentionally only the installed-library smoke gate. It does not create or read input devices and its `gate.json` declares that status. A build with gate `libinput-circular-scroll-v2` is not releasable from that smoke result alone.

The release proof must run the patched upstream litest suite and the LOCachy diskless VM pointer test. Both tests create uinput devices only inside a disposable test target; never run either against the live display.

## Package recipe

The tracked adapter sources the locked Arch recipe, applies `library.patch` and then `circular-scroll.patch` in `prepare()`, retains upstream `build()` and `check()`:

```sh
makepkg --verifysource
makepkg --syncdeps --cleanbuild --noconfirm
```

Do not pass `--nocheck`. The package build and its tests must run inside the disposable Linux test target, not the live system. Upstream `check()` runs `meson test -C build --print-errorlogs`.

## Focused upstream litest proof

To build an already patched source tree on Linux:

```sh
meson setup build -Ddocumentation=false -Ddebug-gui=false -Dtests=true
meson compile -C build
```

Run the focused tests from the same tree, inside the disposable VM:

```sh
build/libinput-test-suite --filter-test='touchpad_circular_scroll_*' --filter-device='LITEST_PANASONIC_CF_FV1'
build/libinput-test-suite --filter-test='quirks_parse_circular_scroll' --filter-device='LITEST_MOUSE'
```

The circular tests cover the default unitless ring boundary, the 300ms dwell and its cancellation, ordinary two-finger scrolling on both axes, and preserved three-finger swipe. `AttrCircularScrollRingWidth` is an unsigned, unitless integer from 1 to 50, default 25; it selects the outer percentage of the reported radius and is not a physical-distance calibration. `AttrCircularScrollDwell` is an unsigned millisecond value from 0 to 2000, default 300. The quirks test covers accepted and rejected values. A missing test runner is a failed release prerequisite, not a reason to fall back to the smoke gate.

## LOCachy VM proof

Use the existing diskless VM script with the actual built library and patched quirks:

```sh
LIBINPUT_SOURCE=PATCHED_SOURCE src/tools/test-pointer-libinput.sh KERNEL UINPUT_MODULE RESULT_DIRECTORY
```

`PATCHED_SOURCE` contains the Meson `build/` directory. `KERNEL` is a readable Linux kernel image, `UINPUT_MODULE` is its matching `uinput.ko` or `.ko.zst`, and `RESULT_DIRECTORY` must not already exist. The script boots QEMU with private devtmpfs and runs the pointer fixture inside the VM. This requires KVM, not a running labwc.

The required proof includes circular plus 2FG methods, pre-dwell no-scroll, post-dwell circular scroll, second-finger cancellation, vertical and horizontal 2FG scroll, and three-finger gestures. The pointer fixture checks combined methods, waits for dwell before circular movement, and verifies both scroll axes in circular mode; the upstream cases separately check boundary and pre/post-dwell cancellation. The script takes exactly three arguments, optional `LIBINPUT_SOURCE` and `PREPARE_ONLY=1`, a kernel and matching `.ko` or `.ko.zst` module, and a new output directory. `PREPARE_ONLY=1` prepares a VM but does not prove runtime behavior.

## Acceptance

A release report must include the installed `gate.json` with id `libinput-circular-scroll-v2` and saved logs from the focused upstream litest and pointer VM commands. A `gate.json` smoke result alone is insufficient evidence for the v2 behavior.

Candidate evidence: a full Linux library and test-suite build completed; a diskless KVM guest passed all 18 focused circular cases and the quirk-boundary case with no skips. A separate pointer-layer VM passed circular clockwise/counterclockwise motion, both directions of both 2FG axes, ordinary gestures and input hotplug. Retain logs tied to the exact patch hash; these results do not by themselves establish real-device usability or final integrated-release acceptance.
