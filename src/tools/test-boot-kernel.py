#!/usr/bin/env python3
"""Boot image kernel selection with a fake package database and no ESP writes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class BootKernel(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="boot-kernel-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.lib = self.root / "lib"
        self.lib.mkdir()
        (self.lib / "init").write_text("fixture")
        (self.lib / "init").chmod(0o755)
        self.rc = self.root / "rc.conf"
        self.rc.write_text("")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        self.image = self.root / "7.3-lts/vmlinuz"
        self.image.parent.mkdir()
        self.image.write_text("kernel")
        pacman = self.bin / "pacman"
        pacman.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >>"$TEST_CALLS"\n'
                          'case "${TEST_MODE:-one}" in missing) exit 1;; '
                          'two) printf "%s\\n%s\\n" "$TEST_IMAGE" "$TEST_IMAGE";; '
                          '*) printf "%s\\n" "$TEST_IMAGE";; esac\n')
        pacman.chmod(0o755)
        script = (SOURCE / "link/locachy-boot").read_text()
        # Execute the actual selection code only; building and ESP writes are
        # deliberately outside this test process.
        stop = script.index('\nmicrocode=')
        script = script[:stop] + '\nprintf "selected=%s version=%s\\n" "$kernel_package" "$version"\n'
        script = script.replace("lib=/usr/local/lib/locachy", "lib=" + str(self.lib))
        script = script.replace("rc=/etc/locachy/rc.conf", "rc=" + str(self.rc))
        script = script.replace("work=$(mktemp -d)", 'work=$(mktemp -d "$TEST_ROOT/work.XXXXXXXX")')
        self.script = self.root / "boot"
        self.script.write_text(script)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TEST_CALLS=str(self.calls), TEST_IMAGE=str(self.image), TEST_ROOT=str(self.root))

    def run_boot(self, selection=None, mode="one"):
        self.rc.write_text('boot_kernel="' + selection + '"\n' if selection is not None else "")
        return subprocess.run(["sh", str(self.script)], env=dict(self.env, TEST_MODE=mode),
                              capture_output=True, text=True, timeout=10)

    def test_default_preserves_existing_kernel(self):
        r = self.run_boot()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("selected=linux-cachyos-bore", r.stdout)

    def test_selection_controls_the_boot_package(self):
        r = self.run_boot("linux-lts")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("selected=linux-lts version=7.3-lts", r.stdout)
        self.assertEqual(self.calls.read_text().strip(), "-Qlq linux-lts")

    def test_bad_names_never_reach_pacman(self):
        for value in ("--help", "linux;bad", "linux-headers", "/vmlinuz", "linux foo"):
            with self.subTest(value=value):
                r = self.run_boot(value)
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(self.calls.exists())

    def test_absent_or_ambiguous_images_fail_without_building(self):
        for mode in ("missing", "two"):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.run_boot("linux-lts", mode).returncode, 0)


if __name__ == "__main__":
    unittest.main()
