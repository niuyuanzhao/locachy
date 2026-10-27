#!/usr/bin/env python3
"""Drive the actual guest through QMP; only its emulated keyboard is used.
  test-root-boot.py TEST_DIRECTORY TIMEOUT_SECONDS
"""
import collections
import json
from pathlib import Path
import re
import select
import socket
import sys
import time

PASSWORD = "Locachy123"  # Fixed nonsecret fixture, never written to a live root.
PLAIN = {" ": "spc", "-": "minus", "=": "equal", "'": "apostrophe",
         ";": "semicolon", ",": "comma", ".": "dot", "/": "slash",
         "\\": "backslash"}
SHIFTED = {'"': "apostrophe", "_": "minus", "+": "equal", ":": "semicolon",
           "?": "slash", "<": "comma", ">": "dot", "!": "1", "*": "8",
           "$": "4", "&": "7", "(": "9", ")": "0"}


def keys_for(char):
    if char.isdigit() or "a" <= char <= "z":
        return [char]
    if "A" <= char <= "Z":
        return ["shift", char.lower()]
    if char in PLAIN:
        return [PLAIN[char]]
    if char in SHIFTED:
        return ["shift", SHIFTED[char]]
    raise ValueError("No test keyboard code for " + repr(char))


def connect(path, deadline):
    while time.monotonic() < deadline:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(str(path))
            return client
        except OSError:
            client.close()
            time.sleep(0.05)
    raise RuntimeError("Guest socket did not appear: " + str(path))


def screenshot_pixels(path):
    # QMP's uncompressed PPM avoids optional image libraries in this harness.
    raw = path.read_bytes()
    match = re.match(br"P6\s+(\d+)\s+(\d+)\s+255\s", raw)
    if not match:
        raise RuntimeError("Invalid QMP screenshot: " + str(path))
    width, height = map(int, match.groups())
    pixels = raw[match.end():]
    if width < 320 or height < 200 or len(pixels) != width * height * 3:
        raise RuntimeError("Incomplete QMP screenshot: " + str(path))
    colors = collections.Counter(pixels[i:i + 3] for i in range(0, len(pixels), 3))
    varied = width * height - colors.most_common(1)[0][1]
    if len(colors) < 3 or varied < 1000:
        raise RuntimeError("Guest screenshot has no rendered UI: " + str(path))
    return {"width": width, "height": height, "colors": len(colors), "non_background_pixels": varied}


class Guest:
    def __init__(self, out, seconds):
        self.out = out
        self.deadline = time.monotonic() + seconds
        self.serial = connect(out / "com1.sock", self.deadline)
        self.qmp = connect(out / "qmp.sock", self.deadline)
        self.log = (out / "serial.log").open("wb")
        self.pending = b""
        self.qmp_buffer = b""
        self.alive = True
        self.read_qmp()  # Greeting, before qmp_capabilities.
        self.command("qmp_capabilities")

    def remaining(self):
        left = self.deadline - time.monotonic()
        if left <= 0:
            raise RuntimeError("Guest acceptance timed out")
        return left

    def read_serial(self):
        data = self.serial.recv(65536)
        if not data:
            self.alive = False
            return
        self.log.write(data)
        self.log.flush()
        self.pending += data

    def pump(self, seconds):
        end = min(self.deadline, time.monotonic() + seconds)
        while self.alive and time.monotonic() < end:
            ready, _, _ = select.select([self.serial], [], [], max(0, end - time.monotonic()))
            if ready:
                self.read_serial()

    def wait_for(self, pattern):
        expression = re.compile(pattern)
        while True:
            match = expression.search(self.pending)
            if match:
                self.pending = self.pending[match.end():]
                return
            if not self.alive:
                raise RuntimeError("Guest ended before " + repr(pattern))
            self.pump(min(0.2, self.remaining()))

    def read_qmp(self):
        while b"\n" not in self.qmp_buffer:
            ready, _, _ = select.select([self.qmp, self.serial] if self.alive else [self.qmp], [], [],
                                         min(5, self.remaining()))
            if self.serial in ready:
                self.read_serial()
            if self.qmp in ready:
                data = self.qmp.recv(65536)
                if not data:
                    raise RuntimeError("QMP closed before replying")
                self.qmp_buffer += data
        line, self.qmp_buffer = self.qmp_buffer.split(b"\n", 1)
        return json.loads(line)

    def command(self, name, arguments=None):
        message = {"execute": name}
        if arguments is not None:
            message["arguments"] = arguments
        self.qmp.sendall(json.dumps(message).encode() + b"\n")
        while True:
            reply = self.read_qmp()
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError("QMP " + name + ": " + str(reply["error"]))

    def key(self, *names):
        self.command("send-key", {"keys": [{"type": "qcode", "data": name} for name in names],
                                  "hold-time": 40})
        self.pump(0.08)

    def type(self, text):
        for char in text:
            self.key(*keys_for(char))

    def shot(self, name):
        self.pump(0.3)
        path = self.out / (name + ".ppm")
        self.command("screendump", {"filename": str(path)})
        return screenshot_pixels(path)

    def close(self):
        self.log.close()
        self.serial.close()
        self.qmp.close()


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: test-root-boot.py TEST_DIRECTORY TIMEOUT_SECONDS")
    out = Path(sys.argv[1]).resolve()
    guest = None
    screenshots = {}
    try:
        guest = Guest(out, float(sys.argv[2]))
        guest.wait_for(br"init: locking")
        guest.wait_for(br"init: started lock \(pid [0-9]+\)")
        # lock sends its ready frame internally, not to COM1. Allow that frame
        # and the real PAM conversation to reach the emulated display.
        guest.pump(1.5)
        screenshots["locked"] = guest.shot("locked")
        guest.type(PASSWORD)
        guest.key("ret")
        guest.wait_for(br"init: unlocked")
        guest.wait_for(br"home> ")
        screenshots["home"] = guest.shot("home")
        # files is always the first builtin; term is the first configured
        # host program. The fixture contains no containers to change indices.
        guest.key("down")
        guest.key("ret")
        guest.wait_for(br"term: started /bin/bash")
        guest.wait_for(br"term: shown 1")
        guest.pump(0.5)
        screenshots["term"] = guest.shot("term")
        guest.type("echo boot-test-terminal-ok >/dev/kmsg")
        guest.key("ret")
        guest.wait_for(br"boot-test-terminal-ok")
        guest.type("exit")
        guest.key("ret")
        guest.wait_for(br"home: ended /usr/local/bin/term 0")
        guest.wait_for(br"home> ")
        screenshots["home-returned"] = guest.shot("home-returned")
        guest.key("end")
        guest.key("ret")
        guest.key("y")
        guest.wait_for(br"init: power off")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
    except (OSError, ValueError, RuntimeError) as error:
        print("FAIL root boot: " + str(error), file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()
    log = (out / "serial.log").read_bytes()
    required = [br"init: keyboards: [1-9][0-9]*,.*root: /dev/sd[a-z][0-9]*, esp: /dev/sd[a-z]1",
                br"modules: .*loaded [1-9][0-9]*, 0 failed", br"screen: .*logical",
                br"boot-test: service-ready", br"init: locking", br"init: unlocked",
                br"term: started /bin/bash", br"boot-test-terminal-ok",
                br"home: ended /usr/local/bin/term 0", br"init: power off"]
    forbidden = [br"Kernel panic", br"screen: none", br"memory BAD", br"cannot switch to the root",
                 br"init: cannot run", br"init: cannot send", br"init: the lock ended \(status [1-9]",
                 br"cannot make / read-only", br"umount /efi:"]
    failures = ["missing " + repr(pattern) for pattern in required if not re.search(pattern, log)]
    failures += ["unexpected " + repr(pattern) for pattern in forbidden if re.search(pattern, log)]
    if (out / "locked.ppm").read_bytes() == (out / "home.ppm").read_bytes():
        failures.append("locked and unlocked screenshots are identical")
    (out / "result.json").write_text(json.dumps({"passed": not failures, "screenshots": screenshots,
                                                "failures": failures}, indent=2) + "\n")
    if failures:
        print("FAIL root boot: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("PASS guest: real root, drivers, rendered lock, PAM unlock, terminal and clean shutdown")
    return 0


if __name__ == "__main__":
    sys.exit(main())
