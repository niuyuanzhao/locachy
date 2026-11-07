#!/usr/bin/env python3
"""Real drags through labwc's drop gate.

Run by test-drop-gate.sh inside its private mount and PID namespaces:
  test-drop-gate.py OUT CGROUP LABWC
The compositor, the containers' security-context listener (labwc-control),
the clients' data devices and the virtual pointer are real. The document
service is a stand-in on its control socket: it records each request with
its peer credentials and answers as the case says.
"""
from pathlib import Path
import os
import socket
import struct
import subprocess
import sys
import threading
import time

OUT = Path(sys.argv[1])
GROUP = Path(sys.argv[2])
LABWC = sys.argv[3]
CONTROL = "/run/locachy/docs/control"
PORTAL = "application/vnd.portal.filetransfer"
URI_LIST = "text/uri-list"
PAYLOAD = "file:///run/user/1000/doc/a1b2c3/report.txt"
LAYOUT = (1280, 720)
SOURCE_AT = (240, 230)
TARGET_AT = (900, 230)
DESKTOP_AT = (640, 620)
FAILURES = 0


def check(label, condition, detail=""):
    global FAILURES
    print(f"{'PASS' if condition else 'FAIL'} {label}" + (f": {detail}" if detail else ""), flush=True)
    if not condition:
        FAILURES += 1


def now():
    return time.monotonic() * 1000


def wait_for(test, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if test():
            return True
        time.sleep(.02)
    return False


def events(log):
    """[(milliseconds, "event args")] of a test-drop-client log."""
    result = []
    for line in log.read_text().splitlines() if log.exists() else []:
        stamp, _, rest = line.partition(" ")
        try:
            result.append((float(stamp), rest))
        except ValueError:
            pass
    return result


def first(log, word):
    return next((t for t, e in events(log) if e == word or e.startswith(word + " ")), None)


def escape():
    """Escape down and up on a virtual keyboard; the time it was sent."""
    sent = now()
    subprocess.run([str(OUT / "client"), "escape"], capture_output=True, timeout=5,
                   env=dict(os.environ, WAYLAND_DISPLAY="wayland-0"), check=True)
    return sent


def ping():
    p = subprocess.run([str(OUT / "client"), "ping", "20"], capture_output=True, text=True, timeout=3,
                       env=dict(os.environ, WAYLAND_DISPLAY="wayland-0"))
    parts = p.stdout.split()
    return float(parts[3]) if p.returncode == 0 and len(parts) == 4 else None


class Docs:
    """The stand-in document service. Modes: ok, refuse, close (no answer,
    closed), never (no answer, open past the gate's timeout), slow (ok after
    looking at the target and the compositor), move (ok after the pointer
    has moved to another client)."""

    def __init__(self):
        Path(CONTROL).parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(CONTROL)
        self.listener.listen(8)
        self.mode = "ok"
        self.requests = []
        self.target_log = None
        self.moved = threading.Event()
        self.asked = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            connection, _ = self.listener.accept()
            threading.Thread(target=self.answer, args=(connection,), daemon=True).start()

    def answer(self, connection):
        with connection:
            pid, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            request = {"text": connection.recv(4096).decode(), "pid": pid, "uid": uid, "at": now(),
                       "mode": self.mode}
            self.requests.append(request)
            self.asked.set()
            mode = self.mode
            if mode in ("slow", "never"):
                request["drops_held"] = sum(e == "drop" for _, e in events(self.target_log))
                request["ping_ms"] = ping()
                request["ping_done"] = now()
            if mode == "slow":
                time.sleep(.1)
                request["drops_before_ok"] = sum(e == "drop" for _, e in events(self.target_log))
                mode = "ok"
            if mode == "move":
                self.moved.wait(3)
                mode = "ok"
            if mode == "ok":
                request["answered"] = now()
                connection.send(b"ok")
            elif mode == "refuse":
                request["answered"] = now()
                connection.send(b"refuse no document view for this container")
            elif mode == "never":
                time.sleep(1)


def start(path, *args, env=None, name=None):
    log = (OUT / f"{name}.log").open("w")
    process = subprocess.Popen([str(path), *map(str, args)], stdout=log, stderr=subprocess.STDOUT,
                               env=env or os.environ)
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


def client(name, mode, app_id, *mimes, container=False, window=None):
    """A container's client: locachy-0, in app_demo. A native window's
    (window: its name, window-PROGRAM-N): the host's own socket, wayland-0,
    in app_WINDOW, as process 1 runs files and view."""
    env = dict(os.environ, WAYLAND_DISPLAY="locachy-0" if container else "wayland-0")
    group = "app_demo" if container else f"app_{window}" if window else None
    procs = GROUP / group / "cgroup.procs" if group else None
    log = (OUT / f"{name}.log").open("w")
    process = subprocess.Popen([str(OUT / "client"), mode, app_id, *mimes], stdout=log,
                               stderr=subprocess.STDOUT, env=env,
                               preexec_fn=(lambda: procs.write_text("0")) if procs else None)
    log.close()
    if not wait_for(lambda: first(OUT / f"{name}.log", "ready") is not None):
        raise RuntimeError(f"{name} did not map its window")
    return process


def command(pointer, text):
    pointer.stdin.write(text + "\n")
    pointer.stdin.flush()
    assert pointer.stdout.readline() == "done\n", text


def pointer_at(x, y):
    return subprocess.Popen([str(OUT / "drag"), "--stream", str(x), str(y), *map(str, LAYOUT)],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def glide(pointer, start, end):
    for i in range(1, 11):
        command(pointer, f"warp {start[0] + (end[0] - start[0]) * i // 10} {start[1] + (end[1] - start[1]) * i // 10}")


def drag(label, mimes=(PORTAL, URI_LIST), mode="ok", source_container=False, target_container=False,
         to=TARGET_AT, before_release=None, during=None, after=None, source_window=None, target_window=None):
    """One drag from the source window to TO. BEFORE_RELEASE runs just before
    the release, DURING right after it, AFTER once the drag has ended, all
    with the pointer. Returns what both sides saw and the requests the
    service got for it."""
    docs.mode = mode
    docs.asked.clear()
    docs.moved.clear()
    before = len(docs.requests)
    source = client(label + "-source", "source", "locachy-drop-source", *mimes, container=source_container,
                    window=source_window)
    target = client(label + "-target", "target", "locachy-drop-target", container=target_container,
                    window=target_window)
    source_log, target_log = OUT / f"{label}-source.log", OUT / f"{label}-target.log"
    docs.target_log = target_log
    pointer = pointer_at(*SOURCE_AT)
    try:
        time.sleep(.2)
        command(pointer, "press")
        if not wait_for(lambda: first(source_log, "drag-start") is not None, 2):
            raise RuntimeError(f"{label}: the drag did not start")
        command(pointer, "wait 150")
        glide(pointer, SOURCE_AT, to)
        command(pointer, "wait 200")
        if before_release:
            before_release(pointer)
        released = now()
        command(pointer, "release")
        if during:
            during(pointer)
        wait_for(lambda: first(source_log, "cancelled") or first(source_log, "finished"), 3)
        time.sleep(.3)
        if after:
            after(pointer)
        pointer.stdin.write("quit\n")
        pointer.stdin.flush()
        pointer.wait(timeout=3)
    finally:
        stop(pointer)
        stop(source)
        stop(target)
    pid = next((int(e.split()[1]) for _, e in events(source_log) if e.startswith("pid ")), None)
    return {"released": released, "source": source_log, "target": target_log, "pid": pid,
            "requests": docs.requests[before:]}


def delivered(r):
    drop, data = first(r["target"], "drop"), next((e for _, e in events(r["target"]) if e.startswith("data ")), "")
    return drop is not None and data == "data " + PAYLOAD and first(r["source"], "finished") is not None


def cancelled(r):
    return first(r["source"], "cancelled") is not None and first(r["target"], "drop") is None


def asked(r, text, labwc_pid):
    if len(r["requests"]) != 1:
        return False, f"requests={[q['text'] for q in r['requests']]}"
    q = r["requests"][0]
    ok = q["text"] == text and q["uid"] == 0 and q["pid"] == labwc_pid
    return ok, f"{q['text']!r} from pid {q['pid']} uid {q['uid']}"


def setup():
    rc = OUT / "etc/labwc/rc.xml"
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_HEADLESS_OUTPUTS="1")
    labwc = start(LABWC, "-d", "-C", rc.parent, "-c", rc, env=env, name="labwc")
    if not wait_for(lambda: Path("/run/user/0/wayland-0").exists()):
        raise RuntimeError("labwc did not start")
    os.environ["WAYLAND_DISPLAY"] = "wayland-0"
    control = start(OUT / "control", name="control")
    if not wait_for(lambda: Path("/run/user/0/locachy-0").exists()):
        raise RuntimeError("labwc-control did not open locachy-0")
    return labwc, control


labwc = control = None
docs = Docs()
try:
    labwc, control = setup()
    lp = labwc.pid

    r = drag("host-ok")
    good, detail = asked(r, f"drop {r['pid']} host", lp)
    check("host target: the compositor asks for drop SOURCE_PID host", good, detail)
    answered = r["requests"][0].get("answered") if r["requests"] else None
    drop = first(r["target"], "drop")
    check("host target: ok delivers the drop and its data", delivered(r) and answered and drop > answered,
          f"answered {answered}, drop {drop}")

    r = drag("container-ok", target_container=True)
    good, detail = asked(r, f"drop {r['pid']} container demo", lp)
    check("container target (locachy-0, app_demo): drop SOURCE_PID container demo", good, detail)
    answered = r["requests"][0].get("answered") if r["requests"] else None
    drop = first(r["target"], "drop")
    check("container target: the drop comes only after ok", delivered(r) and answered and drop > answered,
          f"answered {answered}, drop {drop}")

    r = drag("container-slow", target_container=True, mode="slow")
    q = r["requests"][0] if r["requests"] else {}
    check("while the release is held the target has no drop",
          q.get("drops_held") == 0 and q.get("drops_before_ok") == 0, f"{q.get('drops_held')}, {q.get('drops_before_ok')}")
    check("while the release is held the compositor answers 20 roundtrips",
          q.get("ping_ms") is not None and q["ping_ms"] < 200, f"{q.get('ping_ms')} ms")
    check("a late ok (after 100+ ms) still delivers the drop",
          delivered(r) and first(r["target"], "drop") > q.get("answered", float("inf")))

    r = drag("container-refuse", target_container=True, mode="refuse")
    good, detail = asked(r, f"drop {r['pid']} container demo", lp)
    q = r["requests"][0] if r["requests"] else {}
    gone = first(r["source"], "cancelled")
    check("refuse: the source is cancelled and the target gets no drop",
          good and cancelled(r) and first(r["target"], "leave") is not None and gone >= q["answered"], detail)

    r = drag("container-never", target_container=True, mode="never")
    q = r["requests"][0] if r["requests"] else {}
    gone = first(r["source"], "cancelled")
    waited = gone - q["at"] if gone and q else None
    check("no answer: cancelled after the 300 ms timeout, no drop",
          cancelled(r) and waited is not None and 250 <= waited < 900, f"{waited} ms after the request")
    check("no answer: the compositor kept answering during the wait",
          q.get("ping_ms") is not None and q["ping_ms"] < 200 and gone is not None and q["ping_done"] < gone,
          f"20 roundtrips in {q.get('ping_ms')} ms")
    check("the timeout is logged", "no answer in 300 ms" in (OUT / "labwc.log").read_text())
    time.sleep(1)

    r = drag("container-close", target_container=True, mode="close")
    check("closed without an answer: cancelled, no drop", len(r["requests"]) == 1 and cancelled(r))

    os.rename(CONTROL, CONTROL + ".away")
    try:
        r = drag("no-service", target_container=True)
    finally:
        os.rename(CONTROL + ".away", CONTROL)
    gone = first(r["source"], "cancelled")
    late = gone - r["released"] if gone is not None else None
    check("no document service: cancelled at once, no drop",
          not r["requests"] and cancelled(r) and late < 200, f"{late} ms after the release")

    def move_away(pointer):
        docs.asked.wait(3)
        glide(pointer, TARGET_AT, SOURCE_AT)
        command(pointer, "wait 100")
        docs.moved.set()

    r = drag("moved", target_container=True, mode="move", during=move_away)
    check("a different client under the pointer at ok: cancelled, no drop", len(r["requests"]) == 1 and cancelled(r))

    r = drag("plain-uri-list", mimes=(URI_LIST,), target_container=True)
    check("without the portal type: no request, the drop as before", not r["requests"] and delivered(r))

    r = drag("container-source", source_container=True)
    check("a container source with the portal type: no request, the drop as before",
          not r["requests"] and delivered(r))

    # files as process 1 runs it: a native window, app_window-files-1, root,
    # on wayland-0. A host source: the gate applies.
    r = drag("window-source", source_window="window-files-1", target_container=True)
    good, detail = asked(r, f"drop {r['pid']} container demo", lp)
    answered = r["requests"][0].get("answered") if r["requests"] else None
    drop = first(r["target"], "drop")
    check("a native-window source (app_window-files-1) to a container: drop SOURCE_PID container demo",
          good, detail)
    check("a native-window source: the drop comes only after ok", delivered(r) and answered and drop > answered,
          f"answered {answered}, drop {drop}")

    r = drag("window-target", source_window="window-files-1", target_window="window-view-1")
    good, detail = asked(r, f"drop {r['pid']} host", lp)
    check("a native-window target (app_window-view-1) is a host client: drop SOURCE_PID host", good, detail)
    check("a native-window target: ok delivers the drop", delivered(r))

    r = drag("window-refused", source_window="window-files-1", target_container=True, mode="refuse")
    check("a native-window source refused: cancelled, no drop", len(r["requests"]) == 1 and cancelled(r))

    r = drag("desktop", to=DESKTOP_AT)
    check("dropped on no client: no request, wlroots cancels", not r["requests"] and cancelled(r))

    marks = {}

    def press_escape(pointer):
        marks["escape"] = escape()

    def click_then_escape(pointer):
        glide(pointer, TARGET_AT, (TARGET_AT[0] + 20, TARGET_AT[1] + 20))
        command(pointer, "wait 100")
        marks["click"] = now()
        command(pointer, "press")
        command(pointer, "release")
        command(pointer, "wait 100")
        marks["key"] = escape()
        time.sleep(.2)

    r = drag("escape-plain", mimes=(URI_LIST,), before_release=press_escape, after=click_then_escape)
    gone = first(r["source"], "cancelled")
    check("Escape during a drag: the source is cancelled before the release, the target gets no drop",
          cancelled(r) and marks["escape"] <= gone < r["released"] and first(r["target"], "leave") is not None,
          f"escape {marks['escape']:.1f}, cancelled {gone}, released {r['released']:.1f}")
    clicks = [t for t, e in events(r["target"]) if e == "button 272 pressed" and t > marks["click"]]
    check("after Escape the pointer keeps working: a click reaches the target", len(clicks) == 1,
          f"{len(clicks)} presses")
    keys = [t for t, e in events(r["target"]) if e == "key 1 pressed" and t > marks["key"]]
    check("Escape without a drag still reaches the focused client", len(keys) == 1, f"{len(keys)} presses")

    r = drag("escape-portal", target_container=True, before_release=press_escape)
    check("Escape during a portal drag: no drop request, cancelled, no drop", not r["requests"] and cancelled(r),
          f"requests={[q['text'] for q in r['requests']]}")

    def escape_while_held(pointer):
        docs.asked.wait(3)
        marks["held"] = escape()

    r = drag("escape-held", target_container=True, mode="never", during=escape_while_held)
    q = r["requests"][0] if r["requests"] else {}
    gone = first(r["source"], "cancelled")
    early = gone - q["at"] if gone is not None and q else None
    check("Escape while the release is held: cancelled before the timeout, no drop",
          cancelled(r) and early is not None and early < 250, f"{early} ms after the request")
    check("the Escape cancel is logged", "Escape; drag cancelled" in (OUT / "labwc.log").read_text())
    time.sleep(1)

    check("the compositor is still running", labwc.poll() is None)
finally:
    stop(control)
    stop(labwc)
print(f"DROP_GATE_FAILURES={FAILURES}", flush=True)
raise SystemExit(bool(FAILURES))
