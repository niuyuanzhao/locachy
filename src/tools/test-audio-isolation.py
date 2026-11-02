#!/usr/bin/env python3
"""The containers' sound boundary with the real PipeWire, WirePlumber and
pipewire-pulse, run as this user with the shipped configuration fragments
(link/50-locachy-*.conf) in a private configuration and runtime directory.

No sound or video device is opened: ALSA and BlueZ monitors are off, the
graph has two null nodes (support.null-audio-sink). A client of the
containers' sockets (restricted/pipewire-0, restricted/native) must play and
record, and must not change devices, other streams, metadata or modules; the
host's manager socket keeps full control (wpctl, as locachy-bar uses it).
CONTROL runs show what the fragments change: without them restricted clients
get write access and WirePlumber loads its camera monitors.
"""
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import wave

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
TOOLS = ("pipewire", "wireplumber", "pipewire-pulse", "wpctl", "pw-dump", "pw-cli", "pw-play",
         "pw-record", "pw-link", "pw-metadata", "pactl", "paplay", "parec")

NULL_NODES = """
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "test-sink"
      node.description = "test-sink" media.class = "Audio/Sink" audio.position = [ FL FR ] } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "test-source"
      node.description = "test-source" media.class = "Audio/Source/Virtual" audio.position = [ FL FR ] } }
]
"""
NO_DEVICES = """
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
  }
}
"""


def peak(path):
    with wave.open(str(path)) as w:
        frames = w.readframes(w.getnframes())
        samples = struct.unpack("<%dh" % (len(frames) // 2), frames[:len(frames) // 2 * 2])
        return w.getnframes(), max(map(abs, samples), default=0)


class AudioIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [t for t in TOOLS if not shutil.which(t)]
        if missing:
            raise unittest.SkipTest("not installed: " + " ".join(missing))
        if os.path.exists("/run/pipewire/pipewire-0-manager"):
            # wpctl falls back to the system directory: it would reach that one.
            raise unittest.SkipTest("a system-wide PipeWire runs here (/run/pipewire)")

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="audio-isolation-", dir=SCRATCH))
        runtime = tempfile.TemporaryDirectory(prefix="ai-")  # short: socket paths
        self.addCleanup(runtime.cleanup)
        self.run_dir = Path(runtime.name)
        self.pw = self.run_dir / "pw"
        self.pulse = self.run_dir / "pulse"
        self.user0 = self.run_dir / "user0"
        for d in (self.pw, self.pulse, self.user0):
            d.mkdir(mode=0o700)
        for d in (self.pw, self.pulse):  # as audio-client makes them
            (d / "restricted").mkdir(mode=0o755)
        self.children = []
        self.addCleanup(self.stop_all)
        tone = self.tmp / "tone.wav"
        with wave.open(str(tone), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(b"".join(struct.pack("<hh", v, v) for v in
                                   (int(12000 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(48000 * 4))))
        self.tone = tone

    def config(self, name, access=True, cameras=True):
        conf = self.tmp / name
        dirs = {"pw": conf / "pipewire/pipewire.conf.d", "pulse": conf / "pipewire/pipewire-pulse.conf.d",
                "wp": conf / "wireplumber/wireplumber.conf.d"}
        for d in dirs.values():
            d.mkdir(parents=True, exist_ok=True)
        shipped = [("pw", "50-locachy-sockets.conf"), ("pulse", "50-locachy-pulse.conf")]
        if access:
            shipped.append(("wp", "50-locachy-access.conf"))
        if cameras:
            shipped.append(("wp", "50-locachy-cameras.conf"))
        for where, file in shipped:
            shutil.copy(SOURCE / "link" / file, dirs[where] / file)
        (dirs["pw"] / "90-test-null.conf").write_text(NULL_NODES)
        (dirs["wp"] / "90-test-devices.conf").write_text(NO_DEVICES)
        return conf

    def env(self, conf, **extra):
        env = {"PATH": "/usr/bin", "HOME": str(self.tmp), "XDG_CONFIG_HOME": str(conf),
               "XDG_STATE_HOME": str(self.tmp / "state"), "PIPEWIRE_RUNTIME_DIR": str(self.pw), "LANG": "C"}
        env.update(extra)
        return env

    def spawn(self, name, conf, *args, **extra):
        log = (self.tmp / (name + ".log")).open("ab")
        proc = subprocess.Popen([name, *args], env=self.env(conf, **extra), stdout=log, stderr=subprocess.STDOUT)
        self.children.append((proc, log))
        return proc

    def stop(self, proc):
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    def stop_all(self):
        for proc, log in reversed(self.children):
            if proc.poll() is None:
                self.stop(proc)
            log.close()
        print("audio isolation logs: " + str(self.tmp), flush=True)

    def wait(self, check, what, seconds=15):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if check():
                return
            time.sleep(0.05)
        self.fail("timed out waiting for " + what)

    def run_tool(self, conf, *args, timeout=10, **extra):
        return subprocess.run(list(args), env=self.env(conf, **extra), capture_output=True, text=True, timeout=timeout)

    def restricted(self, conf, *args, timeout=10):
        return self.run_tool(conf, *args, timeout=timeout, PIPEWIRE_REMOTE=str(self.pw / "restricted/pipewire-0"))

    def pulse_tool(self, conf, *args, timeout=10):
        return self.run_tool(conf, *args, timeout=timeout, PULSE_SERVER="unix:" + str(self.pulse / "restricted/native"))

    def start_stack(self, conf):
        self.spawn("pipewire", conf)
        self.wait(lambda: all((self.pw / s).is_socket() for s in
                              ("pipewire-0", "pipewire-0-manager", "restricted/pipewire-0")), "PipeWire sockets")
        wp = self.spawn("wireplumber", conf, "-p", "main-systemwide", GIO_USE_VFS="local", WIREPLUMBER_DEBUG="3")
        self.spawn("pipewire-pulse", conf, PULSE_RUNTIME_PATH=str(self.pulse))
        self.wait(lambda: (self.pulse / "restricted/native").is_socket(), "pipewire-pulse socket")
        self.wait(lambda: "test-sink" in self.run_tool(conf, "wpctl", "status").stdout, "WirePlumber policy")
        return wp

    def node_permissions(self, conf):
        dump = json.loads(self.restricted(conf, "pw-dump").stdout)
        return {o["info"]["props"].get("node.name"): set(o.get("permissions", []))
                for o in dump if o["type"].endswith(":Node")}

    def node_id(self, conf, name):
        dump = json.loads(self.run_tool(conf, "pw-dump").stdout)
        return next(o["id"] for o in dump if o["type"].endswith(":Node") and o["info"]["props"].get("node.name") == name)

    def test_containers_sockets(self):
        conf = self.config("conf")
        wp = self.start_stack(conf)
        modes = {s: (self.pw / s).stat().st_mode & 0o777 for s in ("pipewire-0", "pipewire-0-manager", "restricted/pipewire-0")}
        self.assertEqual(modes, {"pipewire-0": 0o660, "pipewire-0-manager": 0o600, "restricted/pipewire-0": 0o666})
        # Only the containers' sockets are in the directories they get.
        self.assertEqual(sorted(p.name for p in (self.pw / "restricted").iterdir() if p.is_socket()), ["pipewire-0"])
        self.assertEqual(sorted(p.name for p in (self.pulse / "restricted").iterdir()), ["native"])
        print("PASS sockets: pipewire-0 0660, pipewire-0-manager 0600 (upstream units), restricted/pipewire-0 0666; "
              "the restricted directories hold the containers' sockets only", flush=True)

        # Restricted native client: read and execute only, on every node.
        perms = self.node_permissions(conf)
        self.assertIn("test-sink", perms)
        for name, p in perms.items():
            self.assertTrue(p <= {"r", "x"} and "r" in p, (name, p))
        sink = self.node_id(conf, "test-sink")
        self.assertEqual(self.run_tool(conf, "wpctl", "set-volume", str(sink), "0.5").returncode, 0)
        result = self.restricted(conf, "pw-cli", "set-param", str(sink), "Props", "{ mute: true, volume: 0.1 }")
        self.assertIn("Permission denied", result.stdout + result.stderr)
        self.assertEqual(self.run_tool(conf, "wpctl", "get-volume", str(sink)).stdout.strip(), "Volume: 0.50")
        self.restricted(conf, "pw-metadata", str(sink), "target.object", "test-source")
        self.assertNotIn("target.object", self.run_tool(conf, "pw-metadata").stdout)
        print("PASS restricted native client: nodes r-x only; set-param refused (EACCES); metadata not set", flush=True)

        # It plays and records.
        play = subprocess.Popen(["pw-play", "--target", "test-sink", str(self.tone)], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                                env=self.env(conf, PIPEWIRE_REMOTE=str(self.pw / "restricted/pipewire-0")))
        self.wait(lambda: "pw-play:output_FL" in self.run_tool(conf, "pw-link", "-l").stdout and
                  "test-sink:playback_FL" in self.run_tool(conf, "pw-link", "-l").stdout, "playback link")
        recorded = self.tmp / "rec.wav"
        subprocess.run(["timeout", "5", "pw-record", "--target", "test-source", str(recorded)],
                       env=self.env(conf, PIPEWIRE_REMOTE=str(self.pw / "restricted/pipewire-0")))
        self.assertEqual(play.wait(timeout=15), 0)
        self.assertGreater(peak(recorded)[0], 0)
        print("PASS restricted native client plays (linked to the sink) and records (%d frames)" % peak(recorded)[0], flush=True)

        # Pulse clients of the containers: restricted, no modules.
        self.assertIn("Server String", self.pulse_tool(conf, "pactl", "info").stdout)
        for args in (("load-module", "module-null-sink", "sink_name=evil"),
                     ("load-module", "module-native-protocol-tcp", "auth-anonymous=1"),
                     ("unload-module", "1"),
                     ("set-sink-volume", "test-sink", "10%")):
            result = self.pulse_tool(conf, "pactl", *args)
            self.assertNotEqual(result.returncode, 0, args)
            self.assertIn("Access denied", result.stderr, args)
        self.assertEqual(self.run_tool(conf, "wpctl", "get-volume", str(sink)).stdout.strip(), "Volume: 0.50")
        self.assertNotIn("evil", self.run_tool(conf, "wpctl", "status").stdout)
        self.assertEqual(self.pulse_tool(conf, "paplay", "-d", "test-sink", str(self.tone), timeout=15).returncode, 0)
        precorded = self.tmp / "parec.wav"
        # Long enough to connect while other suites load the machine: the
        # check is that frames arrive at all.
        subprocess.run(["timeout", "5", "parec", "-d", "test-source", "--file-format=wav", str(precorded)],
                       env=self.env(conf, PULSE_SERVER="unix:" + str(self.pulse / "restricted/native")))
        self.assertGreater(peak(precorded)[0], 0)
        print("PASS pulse client: load-module/unload-module/set-sink-volume Access denied; paplay plays, parec records", flush=True)

        # The host: root's runtime directory has pipewire-0 (a link, as
        # audio-client makes it); wpctl changes the default sink's volume.
        (self.user0 / "pipewire-0").symlink_to(self.pw / "pipewire-0")
        env = self.env(conf, XDG_RUNTIME_DIR=str(self.user0))
        del env["PIPEWIRE_RUNTIME_DIR"]
        self.assertEqual(subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "0.4"], env=env,
                                        timeout=10).returncode, 0)
        self.assertEqual(subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], env=env, capture_output=True,
                                        text=True, timeout=10).stdout.strip(), "Volume: 0.40")
        print("PASS host wpctl through root's pipewire-0 link: set-volume and get-volume of @DEFAULT_AUDIO_SINK@", flush=True)

        # Measured, not a gate: under upstream's restricted (r-x) a client may
        # still link another client's stream to its own input.
        play = subprocess.Popen(["pw-play", "--target", "test-sink", str(self.tone)], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                                env=self.env(conf, PIPEWIRE_REMOTE=str(self.pw / "restricted/pipewire-0")))
        cross = self.tmp / "cross.wav"
        rec = subprocess.Popen(["timeout", "3", "pw-record", "-P", "{ node.autoconnect = false }", str(cross)],
                               env=self.env(conf, PIPEWIRE_REMOTE=str(self.pw / "restricted/pipewire-0")))
        time.sleep(1)
        linked = [self.restricted(conf, "pw-link", "pw-play:output_" + c, "pw-record:input_" + c).returncode
                  for c in ("FL", "FR")]
        rec.wait(timeout=10)
        play.wait(timeout=15)
        print("INFO restricted pw-link of another stream into its own input: rc %s, recorded peak %d" %
              (linked, peak(cross)[1]), flush=True)

        # Cameras: WirePlumber has no camera monitor.
        log = (self.tmp / "wireplumber.log").read_text(errors="replace")
        self.assertIn("script:client/find-default-access.lua", log)
        self.assertNotIn("script:monitors/v4l2", log)
        self.assertNotIn("script:monitors/libcamera", log)
        print("PASS WirePlumber main-systemwide loads no v4l2/libcamera monitor (hardware.video-capture disabled)", flush=True)

        # CONTROL: the same WirePlumber without the access and camera fragments.
        self.stop(wp)
        (self.tmp / "wireplumber.log").rename(self.tmp / "wireplumber-fixed.log")
        bare = self.config("bare", access=False, cameras=False)
        self.spawn("wireplumber", bare, "-p", "main-systemwide", GIO_USE_VFS="local", WIREPLUMBER_DEBUG="3")
        self.wait(lambda: "script:monitors/v4l2" in (self.tmp / "wireplumber.log").read_text(errors="replace"),
                  "control WirePlumber camera monitor")
        self.wait(lambda: "test-sink" in self.node_permissions(bare), "control restricted view")
        perms = self.node_permissions(bare)["test-sink"]
        self.assertIn("w", perms)
        print("CONTROL without the fragments: WirePlumber loads monitors/v4l2 and gives restricted clients %s "
              "on nodes (Perm.RX of WirePlumber 0.5.18)" % "".join(sorted(perms)), flush=True)


if __name__ == "__main__":
    unittest.main()
