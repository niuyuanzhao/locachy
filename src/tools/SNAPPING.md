# Multi-output window snapping

[中文](SNAPPING.zh.md)

## Inner-edge hold during window drag

Upstream labwc's `screenEdgeStrength` resists a window rectangle's edges, not the cursor at an inner output seam. `pkgbuilds/labwc-locachy/inner-edge-hold.patch` adds an inner-edge cursor hold during interactive floating-window moves:

- While moving a floating window interactively, a cursor crossing a shared output edge pauses on the original output for 750 ms (`<snapping><innerEdgeHold>750</innerEdgeHold></snapping>`; `0` disables it).
- Releasing during the hold snaps the window to the half-screen tile on the original output; continuing past 750 ms crosses into the adjacent output; pulling back cancels the hold immediately.
- Ordinary pointer movement and interactive resizing are unaffected.

## Overview hot corners at output seams

`pkgbuilds/labwc-locachy/corner-edge-hold.patch` holds a diagonal pointer approach crossing an output's top or left boundary within 30 logical pixels of its top-left corner for up to 350 ms so the bar's 75 ms overview hot-corner trigger can fire at shared output edges. Pure-axis motion is captured only on the 1-pixel trigger edge; window moves, resizes, button drags, and fullscreen surfaces are unaffected.

## Tests

```sh
bash tools/test-bar-corner.sh /tmp/locachy-corner-unit
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-inner --inner-edge
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-corner --corner
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-geometry
```
