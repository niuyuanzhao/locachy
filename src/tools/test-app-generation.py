#!/usr/bin/env python3
"""The container generation publication of locachy-app (generation_tool):
a build and a rollback publish upper, work and built as one state change, and
an interrupted publication is completed or undone by recovery, never by an
empty upper. Plain files and directories in a scratch folder: no mounts, root
or network.
  python3 -B tools/test-app-generation.py
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]


def function(name):
    text = (SOURCE / "link/locachy-app").read_text()
    start = text.index(name + "() {")
    return text[start:text.index("\n}", start) + 2]


GENERATION = function("generation_tool")


class Generation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="locachy-generation-")
        self.addCleanup(self.tmp.cleanup)
        self.cbase = Path(self.tmp.name) / "example"
        self.cbase.mkdir()

    def tool(self, op):
        script = "set -euo pipefail\n" + GENERATION + '\ngeneration_tool "$@"\n'
        return subprocess.run(["bash", "-c", script, "bash", op, str(self.cbase)],
                              capture_output=True, text=True, timeout=30)

    def put(self, name, text):
        path = self.cbase / name
        if text is None:
            return
        if text == "DIR":
            path.mkdir(exist_ok=True)
        else:
            path.write_text(text)

    def read(self, name):
        path = self.cbase / name
        if path.is_dir():
            return "DIR"
        return path.read_text() if path.exists() else None

    def inode(self, name):
        path = self.cbase / name
        return str(os.lstat(path).st_ino) if os.path.lexists(path) else "-"

    def test_build_publishes_the_new_generation_and_keeps_the_previous(self):
        self.put("upper", "DIR"); self.put("upper/file", "old")
        self.put("work", "DIR")
        self.put("built", "v1")
        self.put("upper.new", "DIR"); self.put("upper.new/file", "new")
        self.put("work.new", "DIR")
        self.put("built.new", "v2")
        self.assertEqual(self.tool("build").returncode, 0)
        self.assertEqual(self.read("upper/file"), "new")
        self.assertEqual(self.read("upper.prev/file"), "old")
        self.assertEqual(self.read("built"), "v2")
        self.assertEqual(self.read("built.prev"), "v1")
        self.assertIsNone(self.read("generation.state"))
        self.assertIsNone(self.read("upper.new"))
        self.assertEqual(self.read("work"), "DIR")

    def test_incomplete_new_build_is_refused_and_changes_nothing(self):
        self.put("upper", "DIR"); self.put("upper/file", "old")
        self.put("work", "DIR"); self.put("built", "v1")
        self.put("upper.new", "DIR")
        result = self.tool("build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete", result.stderr)
        self.assertEqual(self.read("upper/file"), "old")
        self.assertIsNone(self.read("generation.state"))

    def test_rollback_swaps_back_and_refuses_without_a_previous_build(self):
        self.assertNotEqual(self.tool("rollback").returncode, 0)
        self.put("upper", "DIR"); self.put("upper/file", "new")
        self.put("upper.prev", "DIR"); self.put("upper.prev/file", "old")
        self.put("work", "DIR"); self.put("built", "v2"); self.put("built.prev", "v1")
        self.assertEqual(self.tool("rollback").returncode, 0)
        self.assertEqual(self.read("upper/file"), "old")
        self.assertEqual(self.read("upper.prev/file"), "new")
        self.assertEqual(self.read("built"), "v1")
        self.assertEqual(self.read("built.prev"), "v2")
        self.assertIsNone(self.read("generation.state"))

    def test_interrupted_build_is_completed_by_recovery(self):
        # Stopped after upper moved aside and before upper.new took its place.
        self.put("upper.old", "DIR"); self.put("upper.old/file", "old")
        self.put("upper.new", "DIR"); self.put("upper.new/file", "new")
        self.put("work.new", "DIR"); self.put("built", "v1"); self.put("built.new", "v2")
        self.put("generation.state", "build\n")
        self.assertEqual(self.tool("recover").returncode, 0)
        self.assertEqual(self.read("upper/file"), "new")
        self.assertEqual(self.read("upper.prev/file"), "old")
        self.assertEqual(self.read("built"), "v2")
        self.assertEqual(self.read("built.prev"), "v1")
        self.assertIsNone(self.read("generation.state"))
        self.assertIsNone(self.read("upper.old"))

    def test_interrupted_rollback_is_not_exchanged_twice(self):
        self.put("upper", "DIR"); self.put("upper/file", "new")
        self.put("upper.prev", "DIR"); self.put("upper.prev/file", "old")
        self.put("work", "DIR"); self.put("built", "v2"); self.put("built.prev", "v1")
        state = "rollback {} {} {} {}\n".format(self.inode("upper"), self.inode("upper.prev"),
                                                 self.inode("built"), self.inode("built.prev"))
        self.put("generation.state", state)
        # The upper exchange happened once; the stop came before the build record.
        os.rename(self.cbase / "upper", self.cbase / "swap")
        os.rename(self.cbase / "upper.prev", self.cbase / "upper")
        os.rename(self.cbase / "swap", self.cbase / "upper.prev")
        self.assertEqual(self.read("upper/file"), "old")
        self.assertEqual(self.tool("recover").returncode, 0)
        self.assertEqual(self.read("upper/file"), "old")
        self.assertEqual(self.read("upper.prev/file"), "new")
        self.assertEqual(self.read("built"), "v1")
        self.assertEqual(self.read("built.prev"), "v2")
        self.assertIsNone(self.read("generation.state"))


if __name__ == "__main__":
    unittest.main()
