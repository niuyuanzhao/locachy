#!/usr/bin/env python3
"""Drive a real labwc and measure each drag's committed size and pixels.
Invoked by test-snapping.sh inside its private display namespace."""
from pathlib import Path
import math
import os
import subprocess
import sys
import time

OUT = Path(sys.argv[1])
FAILURES = 0
BLUE = bytes.fromhex("4488cc")


def check(label, condition, detail=""):
    global FAILURES
    print(f"{'PASS' if condition else 'FAIL'} {label}" + (f": {detail}" if detail else ""), flush=True)
    if not condition:
        FAILURES += 1


def call(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True, timeout=15).stdout


def spawn(name, *args):
    log = (OUT / f"{name}.log").open("w")
    process = subprocess.Popen(list(map(str, args)), stdout=log, stderr=subprocess.STDOUT)
    log.close()
    return process


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def wait_for(test, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if test():
            return
        time.sleep(.05)
    raise RuntimeError("display/test client did not become ready")


def capture(label):
    prefix = OUT / label
    call(OUT / "shot", prefix)
    images = {}
    for path in OUT.glob(label + "-HEADLESS-*.ppm"):
        magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
        assert magic == b"P6" and maximum == b"255"
        width, height = map(int, dimensions.split())
        assert len(pixels) == width * height * 3
        images[path.stem.rsplit("-HEADLESS-", 1)[1]] = (width, height, pixels)
        # Keep every screenshot without retaining gigabytes of raw PPMs.
        subprocess.run(["gzip", "-n", "-f", str(path)], check=True, timeout=10)
    assert set(images) == set(outputs)
    return images


def box(image):
    width, height, pixels = image
    x0, y0, x1, y1, count = width, height, -1, -1, 0
    for y in range(height):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        at = row.find(BLUE)
        while at >= 0:
            if at % 3 == 0:
                x = at // 3
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
                count += 1
            at = row.find(BLUE, at + 3)
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1, count) if count else None


def read_outputs():
    found = {}
    for line in call(OUT / "outputs").splitlines():
        name, enabled, x, y, width, height, scale = line.split()
        if enabled == "1":
            s = float(scale)
            found[name.rsplit("-", 1)[1]] = (int(x), int(y), int(width) // s, int(height) // s, s)
    return found


def drag_case(layout, name, output, edge, target, client_decorated, bar):
    mode = "csd" if client_decorated else "ssd"
    label = f"{layout}-{mode}-{name}"
    args = [OUT / "window"] + (["--client-decorated"] if client_decorated else [])
    process = spawn(label, *args)
    try:
        log = OUT / f"{label}.log"
        wait_for(lambda: log.exists() and "size " in log.read_text())
        images = capture(label + "-before")
        initial = {n: box(image) for n, image in images.items()}
        present = [(n, value) for n, value in initial.items() if value]
        assert len(present) == 1, initial
        source, first = present[0]
        sx, sy, _, _, ss = outputs[source]
        # Position is read from the actual surface, not assumed from MoveTo.
        # A normal SSD titlebar is directly above the blue client area.
        start_x = sx + first[0] / ss + 100
        start_y = sy + first[1] / ss + (12 if client_decorated else -12)
        min_x = min(value[0] for value in outputs.values())
        min_y = min(value[1] for value in outputs.values())
        max_x = max(value[0] + value[2] for value in outputs.values())
        max_y = max(value[1] + value[3] for value in outputs.values())
        call(OUT / "drag", round(start_x - min_x), round(start_y - min_y),
             round(target[0] - min_x), round(target[1] - min_y), round(max_x - min_x), round(max_y - min_y))
        time.sleep(.15)
        images = capture(label + "-after")
        actual = {n: box(image) for n, image in images.items()}
        size_lines = [line for line in log.read_text().splitlines() if line.startswith("size ")]
        _, cw, ch = size_lines[-1].split()
        cw, ch = int(cw), int(ch)
        state_lines = [line for line in log.read_text().splitlines() if line.startswith("states")]
        states = set(map(int, state_lines[-1].split()[1:]))
        ox, oy, ow, oh, scale = outputs[output]
        # Actual bar uses nearest Terminus height, then rounds logical reservation up.
        panel = bar
        uw, uh = int(ow), int(oh) - panel
        expected_x = uw // 2 if "right" in edge else 0
        expected_y = panel
        if "bottom" in edge:
            expected_y += uh // 2
        tile_w = uw // 2 if "left" in edge or "right" in edge else uw
        tile_h = uh // 2 if "bottom" in edge or ("top" in edge and edge != "top") else uh
        expected = (expected_x * scale, expected_y * scale, tile_w * scale, tile_h * scale)
        measured = actual[output]
        other = any(value is not None for name, value in actual.items() if name != output)
        tiled = {"left": 5, "right": 6, "top": 7, "bottom": 8}
        required = {1} if edge == "top" else {tiled[e] for e in edge.split("-")}
        decoration = [line for line in log.read_text().splitlines() if line.startswith("decoration ")]
        requested_mode = "decoration 1" if client_decorated else "decoration 2"
        # The client rectangle fits the desired decorated tile. SSD margins are
        # <= 32 logical pixels; exact buffer-to-screenshot size is checked too.
        okay = measured is not None and not other and required <= states and decoration[-1] == requested_mode
        if measured:
            x, y, width, height, count = measured
            ex, ey, ew, eh = expected
            margin = 32 * scale
            okay &= (ex - 2 <= x <= ex + margin and ey - 2 <= y <= ey + margin and
                     ew - margin - 2 <= width <= ew + 2 and eh - margin - 2 <= height <= eh + 2 and
                     abs(width - cw * scale) <= 2 and abs(height - ch * scale) <= 2 and
                     count == width * height)
        check(label, okay, f"pixels={actual}, client={cw}x{ch}, states={sorted(states)}, tile={expected}")
    finally:
        stop(process)


def panel_height(scale):
    sizes = (12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 56, 64)
    want = int(18 * scale + .5)
    best = min(sizes, key=lambda n: (abs(n - want), -n))
    return math.ceil((best * 3 // 2) / scale)


# Coordinates are the shared monitor model's logical coordinates, not physical.
layouts = [
    ("horizontal", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1,1280,0\n"),
    ("fractional", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1.5,1280,0\n"),
    ("negative", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=left,1280x720,1,-1280,0\n"),
    ("vertical", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=above,1280x720,1,0,-720\n"),
    ("three-horizontal", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1,1280,0\n"
                         "HEADLESS-3=right,1280x720,1,2560,0\n"),
    ("three-fractional", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1.5,1280,0\n"
                         "HEADLESS-3=right,1280x720,1.25,2133,0\n"),
]
if os.environ.get("LOCACHY_SNAP_HELPERS_ONLY"):
    layouts = []
lab = control = bar_process = None
try:
    for layout, config in layouts:
        Path("/etc/locachy/monitors.conf").write_text(config)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   WLR_HEADLESS_OUTPUTS=str(len(config.splitlines())))
        log = (OUT / f"{layout}-labwc.log").open("w")
        lab = subprocess.Popen([os.environ.get("LOCACHY_TEST_LABWC", "labwc"), "-d", "-C", "/etc/locachy/labwc", "-c", "/etc/locachy/labwc/rc.xml"],
                               stdout=log, stderr=subprocess.STDOUT, env=env)
        log.close()
        wait_for(lambda: Path("/run/user/0/wayland-0").exists())
        os.environ["WAYLAND_DISPLAY"] = "wayland-0"
        control = spawn(layout + "-control", OUT / "control")
        wait_for(lambda: Path("/run/user/0/locachy-control.sock").exists())
        time.sleep(.3)
        outputs = read_outputs()
        expected_outputs = {}
        for line in config.splitlines():
            name, value = line.split("=")
            _, mode, scale, x, y = value.split(",")
            width, height = map(int, mode.split("x"))
            scale = float(scale)
            expected_outputs[name.rsplit("-", 1)[1]] = (int(x), int(y), width // scale, height // scale, scale)
        check(layout + " output configuration", outputs == expected_outputs, str(outputs))
        bar_process = spawn(layout + "-bar", OUT / "bar")
        time.sleep(.6)
        for decorated in (False, True):
            for n in sorted(outputs):
                x, y, width, height, scale = outputs[n]
                panel = panel_height(scale)
                cases = [
                    ("left", (x + 1, y + height / 2)),
                    ("right", (x + width - 2, y + height / 2)),
                    ("bottom", (x + width / 2, y + height - 2)),
                    ("top", (x + width / 2, y + 1)),
                    ("top-left", (x + 1, y + 1)),
                    ("bottom-right", (x + width - 2, y + height - 2)),
                ]
                for edge, target in cases:
                    drag_case(layout, n + "-" + edge, n, edge, target, decorated, panel)
        stop(bar_process)
        stop(control)
        stop(lab)
        bar_process = control = lab = None
        time.sleep(.1)
finally:
    for process in (bar_process, control, lab):
        stop(process)
if not os.environ.get("LOCACHY_SNAP_HELPERS_ONLY"):
    print(f"SNAPPING_FAILURES={FAILURES}", flush=True)
    raise SystemExit(bool(FAILURES))
