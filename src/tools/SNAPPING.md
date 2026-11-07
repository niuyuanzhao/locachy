# Multi-output window snapping

[中文](SNAPPING.zh.md)

## Report, reproduced cause and fix

The reported failure is **not** exterior-edge tiling or the ability to release
at an exact shared-edge coordinate. During a normal window drag, the pointer
passes straight through the boundary between outputs; there is no edge to push
against while choosing a half-screen tile. The earlier 174 exact-coordinate
geometry checks did not test that complaint and must not be cited as its acceptance.

This was reproduced on labwc 0.20.2 with continuous relative pointer motion on
three real headless outputs. All four directions across the two junctions fail to
hold, for both server-decorated and client-decorated windows. The same final test
without the fix reports **21 PASS, 53 FAIL** (`INNER_EDGE_FAILURES=53`), including
the bounded vertical, mixed-scale and lifecycle cases below.

Upstream's `screenEdgeStrength` resists the **window rectangle's** edges, not the
pointer at an inner snap trigger. `snapping/range/inner` merely recognizes the
pointer's current coordinate, and the overlay delay changes only when a preview
is shown. Neither holds a continuing pointer motion. The relevant upstream code
is `resistance_move_apply` (`resistance.c`), `preprocess_cursor_motion`
(`input/cursor.c`) and `edge_from_cursor` (`interactive.c`).

The tracked `pkgbuilds/labwc-locachy/inner-edge-hold.patch` adds the missing
compositor behavior, rather than a per-machine workaround:

- During an interactive move of a floating window only, a cursor trying to cross
  a genuinely shared output edge pauses on the original output for 750 ms.
- Continuing the drag after that bounded hold crosses to the other output.
  Pulling back cancels the hold immediately. Releasing while held uses labwc's
  existing half-screen snap. Its existing 500 ms preview has time to appear.
- Ordinary pointer movement and interactive resizing are unaffected. Outer edges
  retain the normal output-layout constraint. Nonadjacent outputs and gaps do
  not acquire a fictitious inner edge.
- `<snapping><innerEdgeHold>750</innerEdgeHold></snapping>` is in the patched
  upstream documentation; zero disables it. It also requires a nonzero inner
  snapping range. No new per-machine setting is written.

The adapter applies this small patch after the existing clipboard guard, with
zero fuzz. The immutable input list and patch revision are updated, so normal
Check/Apply rebuilds and validates the tracked stable package. Its behavior
contract is now `labwc-clipboard-corner-drop-v4`, including both inner-edge window
snapping and the hot-corner behavior below, not the older clipboard-only contract.

## Continuous-motion acceptance

Inside a disposable Linux root, with the candidate compositor:

```sh
LOCACHY_TEST_LABWC=/absolute/path/to/candidate-labwc \
  bash tools/test-snapping.sh /absolute/new/inner-result --inner-edge
```

The test builds the current LOCachy bar and monitor controller. Three 1280x720
outputs are configured side by side through the real controller. A virtual
pointer performs repeated **relative** movements, rather than targeting the exact
boundary then releasing. For both directions at both junctions, for both SSD and
CSD clients, it checks:

1. A continuing push into the edge is held (the screenshot's grab-point anchor
   stays at the boundary despite 150 pixels of further input).
2. Before release, the actual snap-preview outline appears on the original side.
3. Release snaps into the original side's half-screen tile.
4. Deliberate continuation after the bounded hold crosses into the next output.
5. The same junctions are crossed immediately by ordinary pointer motion without
   a window drag, verified by pointer events at the receiving client.

Additional bounded cases cover a vertical junction in both directions, a
scale-1/1.5 junction in both directions, immediate retreat/cancel followed by a
fresh drag on the same window, disabling `innerEdgeHold`, and a 1900-pixel event
crossing two junctions (it must stop at the first). An offset-neighbor diagonal
case reproduced an endpoint-check defect in the prototype: the motion crossed
the shared edge segment but ended outside it. The patch now intersects the
motion segment with the edge and holds at that intersection.

The co-patched clipboard+inner-edge candidate passes all 74 checks with zero
failures, whereas stock labwc without the hold patch fails 53 of the 74 checks.

This validates the reproduced compositor defect in an isolated headless setup,
not physical-device acceptance. Physical pointer acceleration and interactive
resize noninterference remain separate hardware checks. The gate also compiles
the bar without Rime when that optional module is absent; both Rime-enabled and
Rime-free builds pass the 74-check suite.

## Separate exact-target geometry regression

```sh
LOCACHY_TEST_LABWC=/absolute/path/to/candidate-labwc \
  bash tools/test-snapping.sh /absolute/new/geometry-result
```

The runner uses two or three private headless outputs, real output management,
current compiled bar with exclusive zones, and actual resizable windows. Real
input devices are hidden; no uinput or physical display is opened. Logs and
screenshots (`.ppm.gz`) are retained.

The matrix covers four two-output layouts (horizontal, scale 1/1.5, negative-X,
and negative-Y vertical), plus three-output horizontal and three-output mixed
scale 1/1.5/1.25 layouts. On every output it drags SSD and CSD windows to left,
right, bottom, top, top-left and bottom-right. It deliberately continues across
any temporary hold to reach the final coordinate; this is **not** the hold test.

Output state readback, xdg-shell dimensions and tiled/maximized states, and
screencopy pixels must agree. The client rectangle must appear only on the
intended output, in the selected tile after decorations and bar are accounted for.
The patched candidate passes all 174 checks (168 drags and six output readbacks);
the two three-screen layouts contribute 72 drags and two configuration checks.

## Overview hot corners at output seams

A different defect affected the upper-left overview trigger. The bar already
used each surface's local logical coordinates, but expected continued negative
relative motion in its one-pixel corner. At an internal output seam, the cursor
crossed into the neighboring output before the 75 ms attempt could complete.
The former claim that this matched KWin was incorrect: KWin's
`ElectricBorderAllScreenCorner` defaults to true.

`corner-edge-hold.patch`, applied after the unchanged `inner-edge-hold.patch`,
provides the missing compositor cooperation. A diagonal pointer approach whose
motion segment crosses the output's top or left boundary within 30 logical
pixels of its top-left corner is held against those two boundaries for at most
350 ms. A pure-axis approach is captured only on the other edge's one-pixel
trigger, never throughout that 30-pixel neighborhood. The other component can still reach the one-pixel trigger. The corner
must actually hit the bar's layer surface; a fullscreen surface covering it
does not reserve a hidden corner. Ordinary horizontal/vertical seam transit,
window moves/resizes, button drags and motion away from the corner pass through.
Reversing direction cancels immediately. Expiry allows crossing and is latched
until the approach has left the corner area, preventing indefinite recapture.

The bar keeps its 75 ms attempt and 275 ms quiet cooldown **per output**, not
across screens. Leaving or destroying that bar clears its incomplete attempt.
Coordinates are logical: negative output origins and fractional scales do not
change the local trigger. No idle timer or periodic cursor polling is added.

```sh
bash tools/test-bar-corner.sh /absolute/new/corner-unit
LOCACHY_TEST_LABWC=/absolute/path/to/candidate-labwc \
  bash tools/test-snapping.sh /absolute/new/corner-result --corner
```

The first command executes the production state machine without a display.
The second runs real bar surfaces and virtual relative pointer input against a
private headless compositor. Only the overview executable is replaced by a
request marker. The 16 checks cover exterior/shared-left/shared-top corners,
fast diagonal overshoot, slow subpixel input, pure-axis trigger-edge approaches,
retreat, ordinary transit, continuous-push cooldown, actual pointer escape after
350 ms, re-entry, negative positions and scale 1.5. The compositor without the
corner patch fails seven of these checks; the candidate passes all 16. The unchanged 74-check inner-edge
snapping suite also passes. These are headless compositor checks, not physical
pointer or real-display acceptance.

## Upstream references

- [labwc-config(5)](https://labwc.github.io/labwc-config.5.html)
- [0.20.2 interactive.c](https://github.com/labwc/labwc/blob/0.20.2/src/interactive.c)
- [0.20.2 input/cursor.c](https://github.com/labwc/labwc/blob/0.20.2/src/input/cursor.c)
- [0.20.2 resistance.c](https://github.com/labwc/labwc/blob/0.20.2/src/resistance.c)
- [0.20.2 view.c](https://github.com/labwc/labwc/blob/0.20.2/src/view.c)
- [Upstream issue 2602](https://github.com/labwc/labwc/issues/2602) increased the
  default snap recognition range from 1 to 10; it did not add a cursor hold.
