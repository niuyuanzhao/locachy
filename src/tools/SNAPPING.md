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
contract is `labwc-clipboard-inner-edge-v2`, not the older clipboard-only contract.

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

On 2026-10-30, the co-patched clipboard+inner-edge candidate passed **74 checks,
zero failures**. Evidence on the development server:

- `~/x230-dev/snap-agent/inner-expanded-final/` and `inner-expanded-final.out`: patched candidate,
  `INNER_EDGE_FAILURES=0`, including screenshots taken while the button is held.
- `~/x230-dev/snap-agent/inner-expanded-stock/` and `inner-expanded-stock.out`: the same
  final test without the hold patch, `INNER_EDGE_FAILURES=53`.
- `~/x230-dev/snap-agent/build-combined.log`: both tracked patches applied with
  zero fuzz and the compositor built. This isolated validation build disables
  Xwayland, icons and SVG, unlike the full packaged build.

This validates the reproduced compositor defect in the isolated candidate, not
physical-device acceptance on the user's machine. Physical pointer acceleration,
interactive-resize noninterference and a full packaged-feature build are not
proved by this reduced compositor prototype run. The gate also compiles the bar
without Rime when that optional module is absent; the actual 74-check inner-edge
suite passed in a disposable overlay with its Rime headers/library removed
(`~/x230-dev/test-snap-no-rime-fd0af365-rerun.log`). No package dependency was
installed to make that test pass. The corresponding Rime-enabled compilation
and 74-check run also pass (`~/x230-dev/test-snap-with-rime-fd0af365.log`);
ELF dependencies confirm the two builds select the intended optional path.

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
The patched candidate passed **174 checks (168 drags and six output readbacks)**:
`~/x230-dev/snap-agent/geometry-patched-final/` and `geometry-patched-final.out`,
`SNAPPING_FAILURES=0`. The two three-screen layouts contribute 72 drags and two
configuration checks.

## Upstream references

- [labwc-config(5)](https://labwc.github.io/labwc-config.5.html)
- [0.20.2 interactive.c](https://github.com/labwc/labwc/blob/0.20.2/src/interactive.c)
- [0.20.2 input/cursor.c](https://github.com/labwc/labwc/blob/0.20.2/src/input/cursor.c)
- [0.20.2 resistance.c](https://github.com/labwc/labwc/blob/0.20.2/src/resistance.c)
- [0.20.2 view.c](https://github.com/labwc/labwc/blob/0.20.2/src/view.c)
- [Upstream issue 2602](https://github.com/labwc/labwc/issues/2602) increased the
  default snap recognition range from 1 to 10; it did not add a cursor hold.
