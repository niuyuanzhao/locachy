#!/usr/bin/env python3
"""Real gate logging wrapper preserves error status and prints inner failures."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class GateErrors(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="gate-errors-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        text = (SOURCE / "link/local-package-gate").read_text()
        start = text.index("        run_logged() {")
        end = text.index('\n        mkdir -p "$out"', start)
        self.script = self.root / "gate"
        self.script.write_text('set -euo pipefail\nname=labwc-locachy\nresult=$1\nshift\n' +
                               text[start:end] + '\nrun_logged host-clients-build.log "$@"\n')

    def test_compiler_error_is_visible_and_exit_status_unchanged(self):
        result = subprocess.run(["bash", str(self.script), str(self.root), "sh", "-c",
                                 'printf "settings.c: error: format-truncation\\n" >&2; exit 7'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 7)
        self.assertIn("labwc-locachy failed (status 7)", result.stderr)
        self.assertIn("settings.c: error: format-truncation", result.stderr)
        self.assertIn(str(self.root / "host-clients-build.log"), result.stderr)
        self.assertEqual((self.root / "host-clients-build.log").read_text(), "settings.c: error: format-truncation\n")

    def test_success_keeps_output_in_its_own_log(self):
        result = subprocess.run(["bash", str(self.script), str(self.root), "sh", "-c", 'printf "build passed\\n"'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout + result.stderr, "")
        self.assertEqual((self.root / "host-clients-build.log").read_text(), "build passed\n")


if __name__ == "__main__":
    unittest.main()
