#!/usr/bin/env python3
"""CPU preflight and optional-IME manifest selection, without root/network.
  python3 -B tools/test-build-matrix.py SCRATCH_PARENT
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class BuildMatrix(unittest.TestCase):
    def test_unsupported_cpu_is_rejected_before_build_side_effects(self):
        text = (SOURCE / "make-root.sh").read_text()
        prefix = text.split('# Canonical absolute paths reject symlinks', 1)[0]
        with tempfile.TemporaryDirectory(prefix="matrix-", dir=SCRATCH) as temp:
            root = Path(temp)
            loader = root / "loader"
            loader.write_text("#!/bin/sh\nprintf '%s\\n' '  x86-64-v3 (supported, searched)' '  x86-64-v2 (supported, searched)'\n")
            loader.chmod(0o755)
            script = root / "preflight.sh"
            script.write_text(prefix.replace("/usr/lib/ld-linux-x86-64.so.2", str(loader)) + "printf 'PREFLIGHT-PASS\\n'\n")
            for level in ("x86-64", "x86-64-v2", "x86-64-v3"):
                result = subprocess.run(["bash", str(script), level], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("PREFLIGHT-PASS", result.stdout)
            result = subprocess.run(["bash", str(script), "x86-64-v4"], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot run x86-64-v4", result.stderr)
            self.assertNotIn("PREFLIGHT-PASS", result.stdout)
            self.assertNotIn("unshare", prefix)
            self.assertNotIn("mkdir", prefix)
            self.assertNotIn("rm -", prefix)

    def test_no_ime_does_not_truncate_unrelated_later_leaves(self):
        text = (SOURCE / "make-root.sh").read_text()
        block = text.split('if [ "${WITH_IME:-1}" = 0 ]; then\n', 1)[1].split('\nfi', 1)[0]
        with tempfile.TemporaryDirectory(prefix="matrix-ime-", dir=SCRATCH) as temp:
            root = Path(temp)
            original = (SOURCE / "packages").read_text() + "\n# Independent later feature\nlater-feature\n"
            for level in ("x86-64", "x86-64-v2", "x86-64-v3", "x86-64-v4"):
                (root / "packages").write_text(original)
                result = subprocess.run(["bash", "-euc", block], env=dict(os.environ, s=str(root)),
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                leaves = {line.split("#", 1)[0].strip() for line in (root / "packages").read_text().splitlines()}
                self.assertTrue({"libinput", "networkmanager", "later-feature"} <= leaves, level)
                self.assertFalse(any(n == "librime" or n == "words" or n.startswith("rime-") for n in leaves), level)


if __name__ == "__main__":
    unittest.main()
