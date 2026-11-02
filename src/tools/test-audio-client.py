#!/usr/bin/env python3
"""Audio service supervision with Unix-socket/tool fixtures, no audio devices.
The kernel log (/dev/kmsg) is a file here; the script copy is changed to it.
The privileged tools (systemd-sysusers, setpriv, install -o, chown) are
fixtures that record what the service asks of them."""
import json
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
SERVERS = ("pipewire", "wireplumber", "pipewire-pulse")


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
        # Socket paths are limited to 107 bytes: the runtime directories are
        # short private ones, whatever the scratch path is.
        runtime = tempfile.TemporaryDirectory(prefix="audio-")
        self.addCleanup(runtime.cleanup)
        self.runtime = Path(runtime.name)
        self.root_runtime = self.runtime / "user0"
        self.root_runtime.mkdir(mode=0o711)
        self.pw_runtime = self.runtime / "pw"
        self.pulse_runtime = self.runtime / "pulse"
        self.state = self.root / "state"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls"
        self.envlog = self.root / "env"
        self.kmsg = self.root / "kmsg"
        self.kmsg.write_text("")
        self.error_path = self.root / "stderr"
        self.error = self.error_path.open("w+")
        self.addCleanup(self.error.close)
        udev = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        udev.bind(str(self.runtime / "udev"))
        udev.listen(1)
        self.addCleanup(udev.close)
        tool = self.bin / "tool"
        tool.write_text('#!' + sys.executable + '\n' + '''import json, os, signal, socket, sys, time
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
def record():
    with open(os.environ["TEST_CALLS"], "a") as out:
        out.write(name + " " + str(os.getpid()) + " " + json.dumps(args) + "\\n")
if name == "install":
    keep, skip = [], False
    for arg in args:
        if skip:
            skip = False
        elif arg in ("-o", "-g"):
            skip = True
        else:
            keep.append(arg)
    record()
    os.execv("/usr/bin/install", ["install"] + keep)
if name == "stat" and not os.environ.get("TEST_STATE_ROOT"):
    os.execv("/usr/bin/stat", ["stat"] + args)
record()
if name == "stat":
    print("0")
    sys.exit(0)
if name == "getent":
    if args == ["passwd", "pipewire"] and not os.environ.get("TEST_NO_USER"):
        print("pipewire:x:990:990:PipeWire system services:/:/usr/bin/nologin")
        sys.exit(0)
    sys.exit(2)
if name == "systemd-sysusers":
    sys.exit(4 if os.environ.get("TEST_SYSUSERS_FAIL") else 0)
if name == "chown":
    sys.exit(0)
if name == "setpriv":
    # The real one changes the user; this one only passes on the command.
    os.execvp(args[args.index("--") + 1], args[args.index("--") + 1:])
if name == "udevadm":
    if os.environ.get("TEST_ENUM_FAIL"):
        print("fixture enumeration failure", file=sys.stderr, flush=True)
        sys.exit(6)
    sys.exit(0)
with open(os.environ["TEST_ENV"], "a") as out:
    out.write(json.dumps({"name": name, "args": args, "env": {k: v for k, v in os.environ.items()
        if k in ("PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH", "XDG_STATE_HOME", "GIO_USE_VFS")}}) + "\\n")
if name == "pipewire" and os.environ.get("TEST_PIPEWIRE_FAIL"):
    print("fixture PipeWire startup failure", file=sys.stderr, flush=True)
    sys.exit(5)
if name == "wireplumber" and os.environ.get("TEST_POLICY_FAIL"):
    print("fixture WirePlumber policy failure", file=sys.stderr, flush=True)
    sys.exit(9)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
if name == "pipewire":
    time.sleep(float(os.environ.get("TEST_PIPEWIRE_DELAY", "0")))
endpoints = []
if name == "pipewire" and not os.environ.get("TEST_PIPEWIRE_NO_SOCKET"):
    endpoints = [Path(os.environ["PIPEWIRE_RUNTIME_DIR"]) / s for s in ("pipewire-0", "pipewire-0-manager", "restricted/pipewire-0")]
if name == "pipewire-pulse":
    endpoints = [Path(os.environ["PULSE_RUNTIME_PATH"]) / "restricted/native"]
for path in endpoints:
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    endpoint.bind(str(path))
    endpoint.listen(1)
while True:
    time.sleep(0.1)
''')
        tool.chmod(0o755)
        for name in ("udevadm", "systemd-sysusers", "getent", "setpriv", "install", "chown", "stat") + SERVERS:
            (self.bin / name).symlink_to(tool.name)
        self.text = (SOURCE / "link/audio-client").read_text()
        for old, new in (("/var/lib/locachy/audio", str(self.state)),
                         ("/run/udev/control", str(self.runtime / "udev")),
                         ("/run/pipewire", str(self.pw_runtime)),
                         ("/run/pulse", str(self.pulse_runtime)),
                         ("/dev/kmsg", str(self.kmsg))):
            self.assertIn(old, self.text)
            self.text = self.text.replace(old, new)
        for name in SERVERS:
            self.text = self.text.replace("/usr/bin/" + name, str(self.bin / name))
        self.script = self.root / "audio-client"
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.root_runtime), TEST_CALLS=str(self.log),
                        TEST_ENV=str(self.envlog), PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
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

    def calls(self, name=None):
        if not self.log.exists():
            return []
        found = []
        for line in self.log.read_text().splitlines():
            tool, pid, args = line.split(" ", 2)
            if name is None or tool == name:
                found.append((tool, int(pid), json.loads(args)))
        return found

    def servers(self):
        if not self.envlog.exists():
            return {}
        return {entry["name"]: entry for entry in map(json.loads, self.envlog.read_text().splitlines())}

    def wait_ready(self, seconds=5):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if (self.pulse_runtime / "restricted/native").is_socket() and "wireplumber" in self.servers() and \
                    (self.root_runtime / "pipewire-0").is_symlink():
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        self.fail("audio fixture did not become ready: " + self.output())

    def assert_children_stopped(self):
        for tool, pid, _ in self.calls():
            if tool not in SERVERS and tool != "setpriv":
                continue
            with self.subTest(child=tool):
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def assert_logged(self, text):
        """In the kernel log, not on the console process 1 gave as stderr."""
        self.assertIn(text, self.kmsg.read_text(), self.output())
        self.error.flush()
        self.assertNotIn(text, self.error_path.read_text())

    def test_servers_run_as_the_system_user_as_upstream_system_units_do(self):
        self.start()
        self.wait_ready()
        # sysusers.d made the user (systemd-sysusers, as at boot).
        self.assertEqual([c[2] for c in self.calls("systemd-sysusers")],
                         [["/usr/local/lib/sysusers.d/locachy-audio.conf"]])
        # User=pipewire, its group and audio (not video), NoNewPrivileges,
        # AmbientCapabilities=CAP_SYS_NICE, no environment of root's.
        privs = [c[2] for c in self.calls("setpriv")]
        self.assertEqual(len(privs), 3, self.output())
        for args in privs:
            head = args[:args.index("--")]
            self.assertEqual(head, ["--reuid=pipewire", "--regid=pipewire", "--groups=audio",
                                    "--inh-caps=+sys_nice", "--ambient-caps=+sys_nice",
                                    "--no-new-privs", "--reset-env"])
        servers = self.servers()
        self.assertEqual(servers["wireplumber"]["args"], ["-p", "main-systemwide"])
        for name in SERVERS:
            self.assertEqual(servers[name]["env"]["PIPEWIRE_RUNTIME_DIR"], str(self.pw_runtime))
        self.assertEqual(servers["pipewire-pulse"]["env"]["PULSE_RUNTIME_PATH"], str(self.pulse_runtime))
        self.assertEqual(servers["wireplumber"]["env"]["XDG_STATE_HOME"], str(self.state))
        self.assertEqual(servers["wireplumber"]["env"]["GIO_USE_VFS"], "local")
        # The runtime directories are the servers' alone; in each, the
        # directory of the containers' socket (restricted, 0755: a container
        # gets the directory, not the socket); the sockets keep the modes
        # PipeWire gave them (no chmod here).
        for directory in (self.pw_runtime, self.pulse_runtime, self.state / "wireplumber"):
            self.assertEqual(directory.stat().st_mode & 0o777, 0o700, directory)
        for directory in (self.pw_runtime / "restricted", self.pulse_runtime / "restricted"):
            self.assertEqual(directory.stat().st_mode & 0o777, 0o755, directory)
        owner = ["-d", "-o", "pipewire", "-g", "pipewire", "-m"]
        self.assertEqual([c[2] for c in self.calls("install") if str(self.pw_runtime) in " ".join(c[2])],
                         [owner + ["0700", str(self.pw_runtime), str(self.pulse_runtime)],
                          owner + ["0755", str(self.pw_runtime / "restricted"), str(self.pulse_runtime / "restricted")]])
        self.assertNotIn("chmod", self.text)
        self.assertEqual(os.readlink(self.root_runtime / "pipewire-0"), str(self.pw_runtime / "pipewire-0"))
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0, self.output())
        self.assert_children_stopped()
        self.assertFalse(os.path.lexists(self.root_runtime / "pipewire-0"))
        self.assertEqual(self.kmsg.read_text(), "")
        print("PASS audio servers run as pipewire (audio group only, CAP_SYS_NICE, no_new_privs), "
              "WirePlumber main-systemwide, runtime directories 0700, the containers' socket directories 0755", flush=True)

    def test_without_its_user_nothing_runs(self):
        for env in ({"TEST_NO_USER": "1"}, {"TEST_SYSUSERS_FAIL": "1"}):
            with self.subTest(env=env):
                if self.log.exists():
                    self.log.unlink()
                self.kmsg.write_text("")
                self.start(**env)
                self.assertEqual(self.process.wait(timeout=5), 1, self.output())
                self.assert_logged("<3>audio-client: no system user pipewire")
                self.assertEqual(self.calls("setpriv"), [])
        print("PASS no system user: the audio servers do not start (never as root)", flush=True)

    def test_state_of_a_root_wireplumber_becomes_its_users_once(self):
        (self.state / "wireplumber").mkdir(parents=True)
        self.start(TEST_STATE_ROOT="1")
        self.wait_ready()
        self.assertEqual([c[2] for c in self.calls("chown")],
                         [["-R", "-h", "pipewire:pipewire", str(self.state / "wireplumber")]])

    def test_policy_failure_is_logged_and_causes_group_cleanup(self):
        self.start(TEST_POLICY_FAIL="1")
        self.assertEqual(self.process.wait(timeout=5), 9, self.output())
        self.assert_logged("fixture WirePlumber policy failure")
        self.assert_logged("<3>audio-client: an audio server ended (status 9)\n")
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
                           str(self.pw_runtime / "pipewire-0") + " (status 5)\n")
        self.assertNotIn("wireplumber", self.servers())
        self.assert_children_stopped()

    def test_live_server_without_socket_fails_after_the_bounded_wait(self):
        self.assertIn("socket_wait=600\n", self.text)
        self.text = self.text.replace("socket_wait=600\n", "socket_wait=20\n")
        self.start(TEST_PIPEWIRE_NO_SOCKET="1")
        self.assertEqual(self.process.wait(timeout=10), 1, self.output())
        self.assert_logged("<3>audio-client: pipewire did not create " + str(self.pw_runtime / "pipewire-0") + " in time\n")
        self.assertNotIn("wireplumber", self.servers())
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
