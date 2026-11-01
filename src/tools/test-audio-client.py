#!/usr/bin/env python3
"""Audio service supervision with Unix-socket/tool fixtures, no audio devices.
The kernel log (/dev/kmsg) is a file here; the script copy is changed to it."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
BASH = shutil.which("bash")


class AudioClient(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run([BASH, "-c", 'printf "%s" "$BASH_VERSION"'], capture_output=True, text=True, check=True)
        if int(result.stdout.split(".", 1)[0]) < 4:
            raise unittest.SkipTest("audio-client supervision requires Bash 4 or newer")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="audio-client-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # Socket paths are limited to 107 bytes: the runtime directory is a
        # short private one, whatever the scratch path is.
        runtime = tempfile.TemporaryDirectory(prefix="audio-")
        self.addCleanup(runtime.cleanup)
        self.runtime = Path(runtime.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls"
        self.kmsg = self.root / "kmsg"
        self.kmsg.write_text("")
        self.error_path = self.root / "stderr"
        self.error = self.error_path.open("w+")
        self.addCleanup(self.error.close)
        for name in ("udev", "bus"):
            value = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            value.bind(str(self.runtime / name))
            value.listen(1)
            self.addCleanup(value.close)
        tool = self.bin / "tool"
        tool.write_text('#!' + sys.executable + '\n' + '''import os, signal, socket, sys, time
from pathlib import Path
name = Path(sys.argv[0]).name
with open(os.environ["TEST_CALLS"], "a") as out:
    out.write(name + " " + str(os.getpid()) + "\\n")
if name == "udevadm":
    if os.environ.get("TEST_ENUM_FAIL"):
        print("fixture enumeration failure", file=sys.stderr, flush=True)
        sys.exit(6)
    sys.exit(0)
if name == "pipewire" and os.environ.get("TEST_PIPEWIRE_FAIL"):
    print("fixture PipeWire startup failure", file=sys.stderr, flush=True)
    sys.exit(5)
if name == "wireplumber" and os.environ.get("TEST_POLICY_FAIL"):
    print("fixture WirePlumber policy failure", file=sys.stderr, flush=True)
    sys.exit(9)
signal.signal(signal.SIGTERM, lambda *args: sys.exit(0))
if name == "pipewire":
    time.sleep(float(os.environ.get("TEST_PIPEWIRE_DELAY", "0")))
if name in ("pipewire", "pipewire-pulse") and not (name == "pipewire" and os.environ.get("TEST_PIPEWIRE_NO_SOCKET")):
    path = Path(os.environ["XDG_RUNTIME_DIR"]) / ("pipewire-0" if name == "pipewire" else "pulse/native")
    path.parent.mkdir(exist_ok=True)
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    endpoint.bind(str(path))
    endpoint.listen(1)
while True:
    time.sleep(0.1)
''')
        tool.chmod(0o755)
        for name in ("udevadm", "dbus-monitor", "pipewire", "wireplumber", "pipewire-pulse"):
            (self.bin / name).symlink_to(tool.name)
        self.text = (SOURCE / "link/audio-client").read_text()
        for old, new in (("/var/lib/locachy/audio", str(self.root / "state")),
                         ("/run/udev/control", str(self.runtime / "udev")),
                         ("/dev/kmsg", str(self.kmsg))):
            self.assertIn(old, self.text)
            self.text = self.text.replace(old, new)
        for name in ("pipewire-pulse", "pipewire", "wireplumber"):
            self.text = self.text.replace("/usr/bin/" + name, str(self.bin / name))
        self.script = self.root / "audio-client"
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.runtime), TEST_CALLS=str(self.log),
                        PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        self.process = None
        self.addCleanup(self.cleanup_process)

    def cleanup_process(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def start(self, **extra):
        self.script.write_text(self.text)
        self.process = subprocess.Popen([BASH, str(self.script)], env=dict(self.env, **extra),
                                        stdout=subprocess.DEVNULL, stderr=self.error)

    def output(self):
        self.error.flush()
        return "kmsg:\n" + self.kmsg.read_text() + "stderr:\n" + self.error_path.read_text()

    def wait_ready(self, seconds=5):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            pulse = self.runtime / "pulse/native"
            if pulse.is_socket() and pulse.stat().st_mode & 0o777 == 0o666 and \
                    self.log.exists() and "wireplumber " in self.log.read_text():
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        self.fail("audio fixture did not become ready: " + self.output())

    def assert_children_stopped(self):
        for line in self.log.read_text().splitlines():
            name, pid = line.split()
            if name == "udevadm":
                continue
            with self.subTest(child=name):
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(pid), 0)

    def assert_logged(self, text):
        """In the kernel log, not on the console process 1 gave as stderr."""
        self.assertIn(text, self.kmsg.read_text(), self.output())
        self.error.flush()
        self.assertNotIn(text, self.error_path.read_text())

    def test_service_uses_runtime_sockets_and_cleans_up_children(self):
        self.start()
        self.wait_ready()
        self.assertEqual((self.runtime / "pipewire-0").stat().st_mode & 0o777, 0o666)
        self.assertEqual((self.runtime / "pulse/native").stat().st_mode & 0o777, 0o666)
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0, self.output())
        self.assert_children_stopped()
        self.assertEqual(self.kmsg.read_text(), "")

    def test_policy_failure_is_logged_and_causes_group_cleanup(self):
        self.start(TEST_POLICY_FAIL="1")
        self.assertEqual(self.process.wait(timeout=5), 9, self.output())
        self.assert_logged("fixture WirePlumber policy failure")
        self.assert_logged("<3>audio-client: an audio server or the session bus watch ended (status 9)\n")
        self.assert_children_stopped()

    def test_slow_server_is_waited_for_not_restarted(self):
        self.start(TEST_PIPEWIRE_DELAY="3")
        self.wait_ready(seconds=15)
        self.assertEqual(self.process.poll(), None)
        self.assertEqual(self.kmsg.read_text(), "")
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0, self.output())
        self.assert_children_stopped()

    def test_ended_server_fails_at_once_with_its_status(self):
        started = time.monotonic()
        self.start(TEST_PIPEWIRE_FAIL="1")
        self.assertEqual(self.process.wait(timeout=10), 5, self.output())
        self.assertLess(time.monotonic() - started, 5)
        self.assert_logged("fixture PipeWire startup failure")
        self.assert_logged("<3>audio-client: pipewire ended before creating " +
                           str(self.runtime / "pipewire-0") + " (status 5)\n")
        self.assertNotIn("wireplumber ", self.log.read_text())
        self.assert_children_stopped()

    def test_live_server_without_socket_fails_after_the_bounded_wait(self):
        self.assertIn("socket_wait=600\n", self.text)
        self.text = self.text.replace("socket_wait=600\n", "socket_wait=20\n")
        self.start(TEST_PIPEWIRE_NO_SOCKET="1")
        self.assertEqual(self.process.wait(timeout=10), 1, self.output())
        self.assert_logged("<3>audio-client: pipewire did not create " + str(self.runtime / "pipewire-0") + " in time\n")
        self.assertNotIn("wireplumber ", self.log.read_text())
        self.assert_children_stopped()

    def test_device_enumeration_failure_is_logged(self):
        self.start(TEST_ENUM_FAIL="1")
        self.wait_ready()
        self.assert_logged("fixture enumeration failure")
        self.assert_logged("<4>audio-client: sound-device enumeration did not finish\n")
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0, self.output())


if __name__ == "__main__":
    unittest.main()
