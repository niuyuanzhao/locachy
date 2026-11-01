#!/usr/bin/env python3
"""Execute make.sh configuration migrations on private files only."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Migration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="container-migration-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        text = (SOURCE / "make.sh").read_text()
        start = text.index("migrate_conf() {")
        end = text.index('\nfor conf in ', start)
        self.script = self.root / "migrate"
        self.script.write_text("#!/bin/sh\nset -eu\n" + text[start:end] + '\nmigrate_conf "$1"\n')
        self.config = self.root / "edge.conf"

    def run_migration(self):
        r = subprocess.run(["sh", str(self.script), str(self.config)], capture_output=True, text=True, timeout=5)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return self.config.read_text()

    def test_shipped_empty_and_missing_arguments_upgrade(self):
        for arguments in ('arguments=""\n', ""):
            before = '# custom note\npackage="microsoft-edge-stable-bin"\ncommand="/usr/bin/microsoft-edge-stable"\nnetwork="0"\n' + arguments
            self.config.write_text(before)
            after = self.run_migration()
            self.assertIn('arguments="--ozone-platform=wayland"', after)
            self.assertIn('network="0"', after)
            self.assertIn('# custom note', after)
            self.assertEqual(after, self.run_migration())

    def test_custom_command_or_arguments_are_unchanged(self):
        for package, command, args in (("microsoft-edge-stable-bin", "/usr/bin/microsoft-edge-stable", "--ozone-platform=x11"),
                                      ("microsoft-edge-stable-bin", "/custom/edge", ""),
                                      ("firefox", "/usr/bin/microsoft-edge-stable", "")):
            before = f'package="{package}"\ncommand="{command}"\narguments="{args}"\n'
            self.config.write_text(before)
            self.assertEqual(self.run_migration(), before)

    def test_duplicate_custom_arguments_are_not_overwritten(self):
        before = 'package="microsoft-edge-stable-bin"\ncommand="/usr/bin/microsoft-edge-stable"\narguments=""\narguments="--custom"\n'
        self.config.write_text(before)
        self.assertEqual(self.run_migration(), before)

    def test_legacy_split_retains_existing_platform(self):
        self.config.write_text('packages="microsoft-edge-stable-bin"\ncommand="/usr/bin/microsoft-edge-stable --ozone-platform=wayland --custom"\n')
        after = self.run_migration()
        self.assertIn('package="microsoft-edge-stable-bin"', after)
        self.assertIn('arguments="--ozone-platform=wayland --custom"', after)
        self.assertEqual(after, self.run_migration())


if __name__ == "__main__":
    unittest.main()
