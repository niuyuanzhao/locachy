# LOCachy libinput patches

[中文](BEHAVIOR.zh.md)

The `libinput` package builds from the official Arch recipe and applies `library.patch` and `circular-scroll.patch` in `prepare()`.

## Circular scrolling

`circular-scroll.patch` adds edge-ring circular scrolling on touchpads:

- `AttrCircularScrollRingWidth`: unsigned integer `1..50` (default `25`), percentage of the touchpad radius forming the outer ring.
- `AttrCircularScrollDwell`: unsigned integer `0..2000` ms (default `300`), dwell time in the ring before circular scrolling starts.
- Two-finger vertical/horizontal scrolling and three-finger gestures remain active alongside circular scrolling.

## Tests

Upstream litest cases:

```sh
meson setup build -Ddocumentation=false -Ddebug-gui=false -Dtests=true
meson compile -C build
build/libinput-test-suite --filter-test='touchpad_circular_scroll_*' --filter-device='LITEST_PANASONIC_CF_FV1'
build/libinput-test-suite --filter-test='quirks_parse_circular_scroll' --filter-device='LITEST_MOUSE'
```

LOCachy pointer test in a diskless VM:

```sh
LIBINPUT_SOURCE=PATCHED_SOURCE src/tools/test-pointer-libinput.sh KERNEL UINPUT_MODULE RESULT_DIRECTORY
```
