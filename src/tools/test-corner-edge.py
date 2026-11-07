#!/usr/bin/env python3
"""Real relative-pointer pushes into exterior and shared output hot corners.

Run by test-snapping.sh OUT --corner in a private headless compositor.
Only the overview executable is a marker; compositor motion, bar surfaces,
relative events and production corner handling are real.
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import time

OUT = Path(sys.argv[1])
os.environ["LOCACHY_SNAP_HELPERS_ONLY"] = "1"
spec = importlib.util.spec_from_file_location("snap", Path(__file__).with_name("test-snapping.py"))
snap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snap)
BASE = Path("/etc/locachy/labwc/rc.xml").read_text()
lab = control = bar = None
outputs = {}
bar_log = None


def last_pointer():
    lines = [line.split() for line in bar_log.read_text().splitlines() if line.startswith("corner-pointer ")]
    assert lines, "bar got no pointer event"
    return int(lines[-1][1]), float(lines[-1][2]), float(lines[-1][3])


def triggers():
    p = OUT / "triggers"
    return len(p.read_text().splitlines()) if p.exists() else 0


def command(p, text):
    p.stdin.write(text + "\n")
    p.stdin.flush()
    assert p.stdout.readline() == "done\n"


def pointer_at(x, y):
    left = min(v[0] for v in outputs.values())
    top = min(v[1] for v in outputs.values())
    width = max(v[0] + v[2] for v in outputs.values()) - left
    height = max(v[1] + v[3] for v in outputs.values()) - top
    return subprocess.Popen([str(OUT / "drag"), "--stream", str(round(x - left)), str(round(y - top)),
                             str(round(width)), str(round(height))], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def close_pointer(p):
    if p is not None and p.poll() is None:
        p.stdin.write("quit\n")
        p.stdin.flush()
        p.wait(timeout=3)


def setup(label, config):
    global lab, control, bar, outputs, bar_log
    for p in (bar, control, lab):
        snap.stop(p)
    lab = control = bar = None
    Path("/etc/locachy/monitors.conf").write_text(config)
    Path("/etc/locachy/labwc/rc.xml").write_text(BASE)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS=str(len(config.splitlines())))
    f = (OUT / (label + "-labwc.log")).open("w")
    lab = subprocess.Popen([os.environ.get("LOCACHY_TEST_LABWC", "labwc"), "-d", "-C", "/etc/locachy/labwc",
                            "-c", "/etc/locachy/labwc/rc.xml"], stdout=f, stderr=subprocess.STDOUT, env=env)
    f.close()
    snap.wait_for(lambda: Path("/run/user/0/wayland-0").exists())
    os.environ["WAYLAND_DISPLAY"] = "wayland-0"
    control = snap.spawn(label + "-control", OUT / "control")
    snap.wait_for(lambda: Path("/run/user/0/locachy-control.sock").exists())
    time.sleep(.25)
    outputs = snap.read_outputs()
    bar = snap.spawn(label + "-bar", OUT / "bar")
    bar_log = OUT / (label + "-bar.log")
    time.sleep(.4)


def push(label, output, dx, dy, steps, delay, initial=12, initial_y=None):
    x, y, *_ = outputs[output]
    count = triggers()
    p = pointer_at(x + initial, y + (initial if initial_y is None else initial_y))
    try:
        command(p, f"push {dx} {dy} {steps} {delay}")
        command(p, "wait 30")
        got = triggers() - count
        snap.check(label, got == 1, f"overview requests={got}")
    finally:
        close_pointer(p)


def pass_through(label, output, dx, dy, yoff=12):
    x, y, *_ = outputs[output]
    count = triggers()
    p = pointer_at(x + 8, y + yoff)
    try:
        command(p, f"push {dx} {dy} 30 5")
        command(p, "wait 30")
        snap.check(label, triggers() == count, f"overview requests={triggers() - count}")
    finally:
        close_pointer(p)


try:
    setup("horizontal", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=right,1280x720,1,1280,0\n")
    push("exterior corner", "1", -3, -3, 12, 12)
    push("internal left seam corner", "2", -3, -3, 12, 12)
    push("fast diagonal corner overshoot", "2", -80, -40, 18, 6)
    push("slow subpixel approach", "2", -.5, -.5, 45, 5)
    pass_through("ordinary horizontal near-corner transit is not captured", "2", -3, 0)
    pass_through("along-seam vertical transit does not activate", "2", 0, -3)
    push("approach horizontally along the top trigger pixel", "2", -3, 0, 15, 12, initial_y=0)
    push("approach vertically along the left trigger pixel", "2", 0, -3, 15, 12, initial=0, initial_y=12)
    pass_through("diagonal transit away from corner does not activate", "2", -3, 3)
    count = triggers()
    x, y, *_ = outputs["2"]
    p = pointer_at(x + 10, y + 10)
    try:
        command(p, "push -3 -3 5 5")
        command(p, "push 4 4 3 5")
        command(p, "wait 120")
        snap.check("retreat cancels incomplete corner attempt", triggers() == count)
    finally:
        close_pointer(p)

    # A continuous horizontal push after approaching the corner must escape
    # the internal seam after 350 ms, rather than being captured forever.
    count = triggers()
    p = pointer_at(x + 10, y + 10)
    origin_surface = last_pointer()[0]
    try:
        command(p, "push -3 -3 12 10")
        command(p, "push -5 0 65 10")
        command(p, "wait 30")
        snap.check("one continuous push requests overview once", triggers() - count == 1,
                   f"requests={triggers() - count}")
        surface, px, py = last_pointer()
        snap.check("350 ms expiry releases the pointer across the seam", surface != origin_surface and px > 900,
                   f"surface {origin_surface}->{surface}, local position={px},{py}")
        # A new approach after retreat is independent of that prior hold.
        command(p, f"warp {x + 12} {y + 12}")
        command(p, "wait 300")
        before = triggers()
        command(p, "push -3 -3 16 10")
        command(p, "wait 30")
        snap.check("expired hold rearms after leaving and approaching again", triggers() == before + 1,
                   f"requests={triggers() - before}")
    finally:
        close_pointer(p)

    setup("vertical", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=below,1280x720,1,0,720\n")
    push("internal top seam corner", "2", -3, -3, 12, 12)
    setup("fractional-negative", "HEADLESS-1=origin,1280x720,1,0,0\nHEADLESS-2=left,1280x720,1.5,-853,160\n")
    push("fractional output corner at negative origin", "2", -3, -3, 12, 12)
    push("origin corner with offset neighbor", "1", -3, -3, 12, 12)
finally:
    for p in (bar, control, lab):
        snap.stop(p)
print(f"CORNER_EDGE_FAILURES={snap.FAILURES}", flush=True)
raise SystemExit(bool(snap.FAILURES))
