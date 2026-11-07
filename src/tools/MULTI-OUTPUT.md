# Native direct-program screens

[中文](MULTI-OUTPUT.zh.md)

Process 1 gives each direct program a DRM lease for the outputs it owns. The
lock owns every available output while locked. Its authentication session and
password buffer are shared; multiple displays do not create additional PAM
transactions or independent unlock states.

`screen_extended(1)` opts into the existing per-output native canvases. Select
an output with `screen_output_select` before laying out text: its row count,
column count, pixel dimensions and font scale then describe that output.
Programs that do not opt in retain the fitted, aspect-preserving mirrored
canvas. Failure to enable native canvases keeps that mirrored mode.

## Lock

The lock lays out the same clock, password prompt, device status and failure
on each native canvas. Short screens remove decorative gaps, then the clock,
date or user label as necessary; the password/error and footer stay separate.
Every pixel of each canvas is cleared, including pixels outside whole text
cells. A new lease (`K_SHOWN`) redraws without clearing the typed password or
restarting authentication. Topology changes do not authenticate or unlock.

## Files

Files has one browser state (tabs, folder contents, selection and operations)
with a native layout on each output, not one first-output screenshot scaled
onto all screens. Auxiliary drawing preserves the active output's scroll
alignment and pending follow-selection request. The input output is drawn
last so the shared hit map names its actual file tiles and buttons.

A pointer event selects its output and rebuilds that output's layout before
hit testing. Desktop logical pixels are converted to local native pixels for
rubberband selection and drag placement. An item drag may move to another
output; a rubberband anchor belongs to its original grid and is cancelled on
crossing. Output/mode/scale changes cancel pixel-dependent drag and pull state,
not the folder or selection. Dialogs remain on the output that opened them;
clicking another output cannot accept their buttons by matching coordinates.

Files temporarily enables native layouts and restores the caller's previous
screen mode and target on exit. This includes file pickers called by settings,
packages or a program owning only one output. Home no longer switches files
back to a first-output mirrored canvas.

## Checks

- `tools/test-lock-layout.c`: two independently sized canvases, compact large
  text, portrait-sized output, removal/reappearance with one auth state, and
  mirrored fallback. No PAM, DRM device or live lock is used.
- `tools/test-lock.c`: private PAM services retain password, fingerprint,
  face-idle and clear/failed-password behavior.
- `tools/test-files.sh`: browser/file operations plus native mixed-size cell
  layouts, output-local hit testing and scaled pointer conversion, rubberband
  crossing, real browse-loop `K_SHOWN` handling and mirror/native transitions;
  plain and AddressSanitizer/UndefinedBehaviorSanitizer builds.

These device-free checks are separate from real KMS/display acceptance. They
do not claim output rotation support: the monitor configuration currently
stores mode, scale and position, not a rotation transform.
