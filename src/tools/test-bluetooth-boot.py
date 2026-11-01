#!/usr/bin/env python3
"""Booted framebuffer/terminal smoke test with a standard BlueZ mock.

Run with test-root-boot.sh. No adapter, wireless pairing, headset or host
input is used. The guest keeps its real PAM lock and normal screen handover.
"""
import importlib.util
import json
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location("root_boot", Path(__file__).with_name("test-root-boot.py"))
root_boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(root_boot)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: test-bluetooth-boot.py TEST_DIRECTORY TIMEOUT_SECONDS")
    out = Path(sys.argv[1]).resolve()
    guest = None
    shots = {}
    try:
        guest = root_boot.Guest(out, float(sys.argv[2]))
        guest.wait_for(br"init: started lock \(pid [0-9]+\)")
        guest.pump(1.5)
        shots["locked"] = guest.shot("locked")
        guest.type(root_boot.PASSWORD)
        guest.key("ret")
        guest.wait_for(br"init: unlocked")
        guest.wait_for(br"home> ")
        guest.key("down")
        guest.key("ret")
        guest.wait_for(br"term: started /bin/bash")
        guest.wait_for(br"term: shown 1")
        guest.type("if busctl --system --no-pager list >/dev/null; then echo bt-boot-bus-ok >/dev/kmsg; fi")
        guest.key("ret")
        guest.wait_for(br"bt-boot-bus-ok")
        guest.type("if bluetui; then echo bt-boot-ui-closed-0 >/dev/kmsg; fi")
        guest.key("ret")
        guest.pump(3)
        shots["no-adapter"] = guest.shot("bluetui-no-adapter")
        guest.key("ctrl", "c")
        guest.wait_for(br"bt-boot-ui-closed-0")
        guest.type("echo bt-boot-terminal-returned >/dev/kmsg")
        guest.key("ret")
        guest.wait_for(br"bt-boot-terminal-returned")
        shots["terminal-returned"] = guest.shot("terminal-returned")
        guest.type("exit")
        guest.key("ret")
        guest.wait_for(br"home: ended /usr/local/bin/term 0")
        guest.wait_for(br"home> ")
        guest.key("end")
        guest.key("ret")
        guest.key("y")
        guest.wait_for(br"init: power off")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
    except (OSError, ValueError, RuntimeError) as error:
        print("FAIL Bluetooth boot: " + str(error), file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()
    log = (out / "serial.log").read_bytes()
    failures = []
    for marker in (b"Kernel panic", b"screen: none", b"init: cannot run", b"AddressSanitizer"):
        if marker in log:
            failures.append("unexpected " + repr(marker))
    if (out / "bluetui-no-adapter.ppm").read_bytes() == (out / "terminal-returned.ppm").read_bytes():
        failures.append("bluetui and the returned shell have identical pictures")
    (out / "result.json").write_text(json.dumps({"passed": not failures, "screenshots": shots,
        "failures": failures,
        "scope": "Real PAM, framebuffer term, packaged bluetui, private standard BlueZ mock with no adapter; no radio"},
        indent=2) + "\n")
    if failures:
        print("FAIL Bluetooth boot: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("PASS Bluetooth boot: real PAM unlock, framebuffer term runs bluetui and returns cleanly; no physical adapter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
