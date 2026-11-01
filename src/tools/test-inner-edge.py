#!/usr/bin/env python3
"""Continuous inner-edge pushes, not exact-coordinate releases. Run through
 test-snapping.sh OUT --inner-edge in its private display namespace."""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import time

OUT = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("snap", Path(__file__).with_name("test-snapping.py"))
snap = importlib.util.module_from_spec(spec)
os.environ["LOCACHY_SNAP_HELPERS_ONLY"] = "1"
spec.loader.exec_module(snap)
BASE = Path("/etc/locachy/labwc/rc.xml").read_text()
lab = controller = bar = None
outputs = {}


def command(process, text):
    process.stdin.write(text + "\n")
    process.stdin.flush()
    assert process.stdout.readline() == "done\n"


def close_motion(process):
    if process is not None:
        if process.poll() is None:
            process.stdin.write("quit\n")
            process.stdin.flush()
        snap.stop(process)


def bounds():
    left = min(v[0] for v in outputs.values())
    top = min(v[1] for v in outputs.values())
    right = max(v[0] + v[2] for v in outputs.values())
    bottom = max(v[1] + v[3] for v in outputs.values())
    return left, top, right - left, bottom - top


def motion_at(x, y):
    left, top, width, height = bounds()
    return subprocess.Popen([str(OUT / "drag"), "--stream", str(round(x - left)), str(round(y - top)),
                             str(round(width)), str(round(height))], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def put_window(label, gx, gy, decorated, disabled=False):
    # Set exact initial client top-left, then read actual pixels before using it.
    xml = BASE.replace('x="300" y="200"', f'x="{round(gx - 100)}" y="{round(gy - 12)}"')
    if disabled:
        xml = xml.replace("</openbox_config>", "<snapping><innerEdgeHold>0</innerEdgeHold></snapping></openbox_config>")
    Path("/etc/locachy/labwc/rc.xml").write_text(xml)
    lab.send_signal(1)
    time.sleep(.08)
    window = snap.spawn(label, OUT / "window", *(["--client-decorated"] if decorated else []))
    log = OUT / f"{label}.log"
    snap.wait_for(lambda: log.exists() and "size " in log.read_text())
    return window, log


def rectangle(images, name):
    area = snap.box(images[name])
    if area is None:
        return None
    x, y, _, _, scale = outputs[name]
    return x + area[0] / scale, y + area[1] / scale, area[2] / scale, area[3] / scale


def grab_point(images, decorated):
    # Prefer an output that contains the left/top edge: clipping into a
    # neighboring output must not be mistaken for the true client origin.
    rects = [r for name in images if (r := rectangle(images, name)) is not None]
    assert rects
    left = min(r[0] for r in rects)
    top = min(r[1] for r in rects)
    return left + 100, top + (12 if decorated else -12)


def setup(label, config):
    global lab, controller, bar, outputs
    for process in (bar, controller, lab):
        snap.stop(process)
    bar = controller = lab = None
    Path("/etc/locachy/monitors.conf").write_text(config)
    Path("/etc/locachy/labwc/rc.xml").write_text(BASE)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS=str(len(config.splitlines())))
    logfile = (OUT / f"{label}-labwc.log").open("w")
    lab = subprocess.Popen([os.environ.get("LOCACHY_TEST_LABWC", "labwc"), "-d", "-C", "/etc/locachy/labwc",
                            "-c", "/etc/locachy/labwc/rc.xml"], stdout=logfile, stderr=subprocess.STDOUT, env=env)
    logfile.close()
    snap.wait_for(lambda: Path("/run/user/0/wayland-0").exists())
    os.environ["WAYLAND_DISPLAY"] = "wayland-0"
    controller = snap.spawn(label + "-control", OUT / "control")
    snap.wait_for(lambda: Path("/run/user/0/locachy-control.sock").exists())
    time.sleep(.25)
    outputs = snap.read_outputs()
    snap.outputs = outputs
    expected = {}
    for line in config.splitlines():
        name, value = line.split("=")
        _, mode, scale, x, y = value.split(",")
        width, height = map(int, mode.split("x"))
        scale = float(scale)
        expected[name.rsplit("-", 1)[1]] = (int(x), int(y), width // scale, height // scale, scale)
    snap.check(label + " output state", outputs == expected, str(outputs))
    bar = snap.spawn(label + "-bar", OUT / "bar")
    time.sleep(.35)


def push_case(label, origin, direction, vertical=False, decorated=True, action="snap", disabled=False):
    x, y, width, height, scale = outputs[origin]
    boundary = (y + height if direction > 0 else y) if vertical else (x + width if direction > 0 else x)
    gx, gy = (x + width / 2, boundary - direction * 180) if vertical else (boundary - direction * 180, y + height / 2)
    window, log = put_window(label, gx, gy, decorated, disabled)
    motion = None
    try:
        images = snap.capture(label + "-before")
        gx, gy = grab_point(images, decorated)
        motion = motion_at(gx, gy)
        command(motion, "press")
        start = gy if vertical else gx
        reach = (boundary - direction * 30 - start) / 10
        command(motion, f"push {0 if vertical else reach} {reach if vertical else 0} 10 5")
        command(motion, f"push {0 if vertical else direction * 5} {direction * 5 if vertical else 0} 30 10")
        held = snap.capture(label + "-held")
        anchor = grab_point(held, decorated)[1 if vertical else 0]
        snap.check(label + (" disabled crosses" if disabled else " continuous push held"),
                   (anchor - boundary) * direction > 70 if disabled else abs(anchor - boundary) <= 5,
                   f"anchor={anchor:.1f}, boundary={boundary}")
        if disabled:
            command(motion, "release")
            return
        if action == "cross":
            command(motion, "wait 800")
            command(motion, f"push {0 if vertical else direction * 5} {direction * 5 if vertical else 0} 42 10")
            crossed = snap.capture(label + "-crossed")
            anchor = grab_point(crossed, decorated)[1 if vertical else 0]
            snap.check(label + " deliberate continuation crosses", (anchor - boundary) * direction > 100,
                       f"anchor={anchor:.1f}, boundary={boundary}")
            command(motion, "release")
            return
        if action == "retreat":
            command(motion, f"push {0 if vertical else -direction * 15} {-direction * 15 if vertical else 0} 4 2")
            retreated = snap.capture(label + "-retreat")
            anchor = grab_point(retreated, decorated)[1 if vertical else 0]
            snap.check(label + " retreat cancels immediately", (anchor - boundary) * direction < -40,
                       f"anchor={anchor:.1f}, boundary={boundary}")
            command(motion, "release")
            # Reuse the same window and pointer immediately: a fresh drag
            # must get its own hold, not an expired previous drag's state.
            point = grab_point(retreated, decorated)
            left, top, _, _ = bounds()
            command(motion, f"warp {point[0] - left} {point[1] - top}")
            command(motion, "press")
            command(motion, f"push {0 if vertical else direction * 5} {direction * 5 if vertical else 0} 25 5")
            renewed = snap.capture(label + "-new-drag")
            anchor = grab_point(renewed, decorated)[1 if vertical else 0]
            snap.check(label + " new drag has fresh hold", abs(anchor - boundary) <= 5,
                       f"anchor={anchor:.1f}, boundary={boundary}")
            command(motion, "release")
            return
        command(motion, "wait 300")
        preview = snap.capture(label + "-preview")
        # Observe the outline away from the dragged window. Horizontal
        # previews expose their vertical middle-side line; vertical ones
        # expose the midpoint horizontal line.
        px = round(width * scale * .5) if not vertical and direction > 0 else 0
        py = round(100 * scale)
        if vertical:
            px = round(100 * scale)
            py = round((snap.panel_height(scale) + int(height - snap.panel_height(scale)) // 2) * scale) if direction > 0 else round(snap.panel_height(scale) * scale)
        image = preview[origin]
        at = (py * image[0] + px) * 3
        pixel = image[2][at:at + 3]
        snap.check(label + " preview before release", pixel != b"\0\0\0", pixel.hex())
        command(motion, "release")
        time.sleep(.1)
        snapped = snap.capture(label + "-snapped")
        states = [line for line in log.read_text().splitlines() if line.startswith("states")]
        states = set(map(int, states[-1].split()[1:]))
        required = (8 if direction > 0 else 1) if vertical else (6 if direction > 0 else 5)
        snap.check(label + " release snaps original side", required in states and snap.box(snapped[origin]) is not None and
                   all(snap.box(image) is None for n, image in snapped.items() if n != origin), str(sorted(states)))
    finally:
        close_motion(motion)
        snap.stop(window)


def ordinary_case(label, origin, direction):
    x, y, width, _, _ = outputs[origin]
    boundary = x + width if direction > 0 else x
    target = boundary + direction * 100
    window, log = put_window(label, target, 312, True)
    motion = None
    try:
        motion = motion_at(boundary - direction * 30, 350)
        command(motion, f"push {direction * 5} 0 26 1")
        time.sleep(.03)
        events = [line for line in log.read_text().splitlines() if line.startswith("pointer ")]
        final = float(events[-1].split()[1]) if events else -1
        snap.check(label + " ordinary pointer crosses immediately", abs(final - 100) <= 1, f"client x={final}")
    finally:
        close_motion(motion)
        snap.stop(window)


def large_delta_case(label, diagonal=False):
    # Middle output only shares y=[200,600) with the left output in the
    # diagonal test. The segment crosses y=430 but its endpoint y=710 is
    # outside that shared interval: endpoint-based detection must fail.
    gx, gy = (1180, 390) if diagonal else (1100, 350)
    window, _ = put_window(label, gx, gy, True)
    motion = None
    try:
        before = snap.capture(label + "-before")
        gx, gy = grab_point(before, True)
        motion = motion_at(gx, gy)
        command(motion, "press")
        command(motion, "push 2 0 1 2") # establish the interactive move
        command(motion, "push 800 320 1 2" if diagonal else "push 1900 0 1 2")
        held = snap.capture(label + "-held")
        anchor_x, anchor_y = grab_point(held, True)
        snap.check(label + " stops at first shared edge", abs(anchor_x - 1280) <= 5,
                   f"anchor={anchor_x:.1f},{anchor_y:.1f}")
        command(motion, "release")
    finally:
        close_motion(motion)
        snap.stop(window)


horizontal = "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1,1280,0\nHEADLESS-3=right,1280x720,1,2560,0\n"
try:
    setup("horizontal", horizontal)
    for decorated in (False, True):
        for origin, direction in (("1", 1), ("2", -1), ("2", 1), ("3", -1)):
            for action in ("snap", "cross"):
                push_case(f"horizontal-{'csd' if decorated else 'ssd'}-{origin}-{direction}-{action}",
                          origin, direction, decorated=decorated, action=action)
    for origin, direction in (("1", 1), ("2", -1), ("2", 1), ("3", -1)):
        ordinary_case(f"ordinary-{origin}-{direction}", origin, direction)
    push_case("retreat-new-drag", "1", 1, action="retreat")
    push_case("disabled", "1", 1, disabled=True)
    large_delta_case("large-delta")
    setup("vertical", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=below,1280x720,1,0,720\n")
    for origin, direction in (("1", 1), ("2", -1)):
        for action in ("snap", "cross"):
            push_case(f"vertical-{origin}-{direction}-{action}", origin, direction, vertical=True, action=action)
    setup("mixed", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1.5,1280,0\n")
    for origin, direction in (("1", 1), ("2", -1)):
        for action in ("snap", "cross"):
            push_case(f"mixed-{origin}-{direction}-{action}", origin, direction, action=action)
    setup("diagonal", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1.5,1280,200\n")
    large_delta_case("diagonal-segment", diagonal=True)
finally:
    for process in (bar, controller, lab):
        snap.stop(process)
print(f"INNER_EDGE_FAILURES={snap.FAILURES}", flush=True)
raise SystemExit(bool(snap.FAILURES))
