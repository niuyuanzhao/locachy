#!/usr/bin/env python3
"""labwc's xinitrc (link/labwc-xinitrc) with command doubles; no display required."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]


class Xinitrc(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="locachy-xwayland-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.calls = self.root / "calls"
        # Xwayland's socket of display 23 as wlroots makes it (labwc's umask):
        # a private copy of the script looks for it here, not in /tmp/.X11-unix.
        self.sockets = self.root / "x11"
        self.sockets.mkdir()
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(self.sockets / "X23"))
        self.addCleanup(self.listener.close)
        os.chmod(self.sockets / "X23", 0o755)
        self.script = self.root / "xinitrc"
        text = (SOURCE / "link/labwc-xinitrc").read_text()
        self.assertIn("socket=/tmp/.X11-unix/X$n", text)
        self.script.write_text(text.replace("socket=/tmp/.X11-unix/", "socket=%s/" % self.sockets))
        for name, content in {
            "getent": '#!/bin/sh\n[ "${TEST_NO_USER:-0}" = 0 ] || exit 2\n'
                      'printf "test-user:x:1000:1000::/home/test-user:/bin/bash\\n"\n',
            "timeout": '#!/bin/sh\nshift\nexec "$@"\n',
            "xhost": '#!/bin/sh\nprintf "%s %s\\n" "$DISPLAY" "$*" >>"$TEST_CALLS"\nexit "${TEST_XHOST_STATUS:-0}"\n',
        }.items():
            path = self.bin / name
            path.write_text(content)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        XDG_RUNTIME_DIR=str(self.runtime), TEST_CALLS=str(self.calls))
        self.env.pop("DISPLAY", None)

    def run_xinitrc(self, display=None, status=0):
        """As labwc runs it (sh FILE), with DISPLAY as labwc sets it."""
        env = dict(self.env, TEST_XHOST_STATUS=str(status))
        if display is not None:
            env["DISPLAY"] = display
        result = subprocess.run(["sh", str(self.script)], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path = self.runtime / "locachy-x11-display"
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(self.runtime.iterdir()), [path])
        return path.read_text()

    def test_display_published_after_the_user_is_admitted(self):
        self.assertEqual(self.run_xinitrc(":23"), ":23\n")
        self.assertEqual(self.calls.read_text(), ":23 +si:localuser:test-user\n")

    def test_the_socket_is_opened_to_every_user_as_an_x_server_does(self):
        self.run_xinitrc(":23")
        self.assertEqual((self.sockets / "X23").stat().st_mode & 0o777, 0o777)

    def test_no_socket_publishes_no_x11(self):
        self.assertEqual(self.run_xinitrc(":24"), "\n")
        self.assertFalse(self.calls.exists())

    def test_a_restarted_xwayland_admits_the_user_again(self):
        self.assertEqual(self.run_xinitrc(":23"), ":23\n")
        self.assertEqual(self.run_xinitrc(":23"), ":23\n")
        self.assertEqual(self.calls.read_text().count("+si:localuser:test-user"), 2)

    def test_no_display_publishes_no_x11(self):
        self.assertEqual(self.run_xinitrc(), "\n")
        self.assertFalse(self.calls.exists())

    def test_failed_admission_publishes_no_x11(self):
        self.assertEqual(self.run_xinitrc(":23", status=1), "\n")

    def test_missing_user_publishes_no_x11(self):
        self.env["TEST_NO_USER"] = "1"
        self.assertEqual(self.run_xinitrc(":23"), "\n")
        self.assertFalse(self.calls.exists())

    def test_remote_and_malformed_displays_never_reach_xhost(self):
        for value in ("localhost:23", ":23.0", ":23;touch bad", ":", ":999999", ":23\n:24", "unix:0", ":-1"):
            with self.subTest(display=value):
                self.assertEqual(self.run_xinitrc(value), "\n")
                self.assertFalse(self.calls.exists())

    def test_a_stale_record_is_replaced(self):
        self.assertEqual(self.run_xinitrc(":23"), ":23\n")
        self.assertEqual(self.run_xinitrc(), "\n")

    def test_installed_with_a_persistent_xwayland_and_its_packages(self):
        rc = (SOURCE / "link/labwc-rc.xml").read_text()
        self.assertIn("<xwaylandPersistence>yes</xwaylandPersistence>", rc)
        self.assertIn('"$src/link/labwc-xinitrc" /etc/locachy/labwc/xinitrc', (SOURCE / "make.sh").read_text())
        packages = json.loads((SOURCE / "link/modules.json").read_text())["desktop"]["packages"]
        for package in ("xorg-xwayland", "xorg-xhost"):
            self.assertIn(package, packages)

    def test_settings_installs_the_same_display_packages(self):
        """settings' "Wayland display (labwc)" module installs what modules.json's desktop has."""
        settings = (SOURCE / "progs/settings.c").read_text()
        row = settings[settings.index('N_("Wayland display (labwc)")'):]
        row = row[:row.index("},")]
        for package in ("xorg-xwayland", "xorg-xhost"):
            self.assertIn(package, row)


if __name__ == "__main__":
    unittest.main()
