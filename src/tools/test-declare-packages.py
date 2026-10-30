#!/usr/bin/env python3
"""declare-packages: what LOCachy's base list and modules add is declared in
the manifest once, for the modules a system has; nothing is installed.
  python3 -B tools/test-declare-packages.py SCRATCH_PARENT
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("declare_packages", str(SOURCE / "link/declare-packages"))
spec = importlib.util.spec_from_loader(loader.name, loader)
declare = importlib.util.module_from_spec(spec)
loader.exec_module(declare)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class DeclarePackages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="declare-packages-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "src"
        (self.src / "link").mkdir(parents=True)
        self.manifest = self.root / "packages"
        self.record = self.root / "state/declared-packages.json"
        self.define(["base", "linux-default"], {"desktop": ["labwc-locachy", "swaybg"],
                                                "view": ["libmupdf", "libarchive"],
                                                "audio": ["pipewire", "wiremix"]})
        (self.src / declare.BASELINE).write_text(json.dumps(
            {"base": ["base", "linux-default"],
             "modules": {"desktop": ["labwc-locachy", "swaybg"], "view": ["libmupdf", "libarchive"],
                         "audio": ["pipewire", "wiremix"]}}))

    def define(self, base, modules):
        (self.src / "packages").write_text("# the base\n" + "".join(p + "\n" for p in base))
        (self.src / "link/modules.json").write_text(json.dumps({n: {"packages": p} for n, p in modules.items()}))

    def run_declare(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return declare.declare(self.src, self.manifest, self.record)

    def leaves(self):
        return declare.leaves(self.manifest.read_text())

    def test_additions_of_declared_modules_once_and_removal_stays(self):
        # A system from before: its kernel swapped, the desktop and view modules on, audio off.
        self.manifest.write_text("base\nlinux-lts\nlabwc-locachy\nswaybg\nlibmupdf libarchive\n")
        self.define(["base", "linux-default", "sbctl"],
                    {"desktop": ["labwc-locachy", "xorg-xwayland", "swaybg"],
                     "view": ["libmupdf", "libarchive", "libraw", "chmlib"],
                     "audio": ["pipewire", "wiremix", "sof-firmware"]})
        groups = self.run_declare()
        self.assertEqual(groups, [("base", ["sbctl"]), ("module desktop", ["xorg-xwayland"]),
                                  ("module view", ["libraw", "chmlib"])])
        text = self.manifest.read_text()
        self.assertIn("# Added by a LOCachy update: module view\nlibraw\nchmlib\n", text)
        self.assertNotIn("linux-default", self.leaves())  # the swapped-out kernel stays out
        self.assertNotIn("sof-firmware", self.leaves())  # audio is not on this system
        self.assertEqual(json.loads(self.record.read_text())["modules"]["view"],
                         ["libmupdf", "libarchive", "libraw", "chmlib"])
        self.assertEqual(self.run_declare(), [])
        self.assertEqual(self.manifest.read_text(), text)
        # The user removes libraw: the next run leaves it removed.
        self.manifest.write_text(text.replace("libraw\n", ""))
        self.assertEqual(self.run_declare(), [])
        self.assertNotIn("libraw", self.leaves())
        # A later addition to the view module is declared, once.
        self.define(["base", "linux-default", "sbctl"],
                    {"desktop": ["labwc-locachy", "xorg-xwayland", "swaybg"],
                     "view": ["libmupdf", "libarchive", "libraw", "chmlib", "djvulibre"],
                     "audio": ["pipewire", "wiremix", "sof-firmware"]})
        self.assertEqual(self.run_declare(), [("module view", ["djvulibre"])])
        self.assertNotIn("libraw", self.leaves())

    def test_module_turned_on_later_counts_from_then(self):
        self.manifest.write_text("base\nlinux-default\n")
        self.define(["base", "linux-default"], {"desktop": ["labwc-locachy", "swaybg"],
                                                "view": ["libmupdf", "libarchive", "libraw"],
                                                "audio": ["pipewire", "wiremix"]})
        self.assertEqual(self.run_declare(), [])
        self.assertEqual(self.leaves(), ["base", "linux-default"])
        # Turned on (settings declares its packages), then the module grows.
        self.manifest.write_text("base\nlinux-default\nlibmupdf\nlibarchive\nlibraw\n")
        self.define(["base", "linux-default"], {"desktop": ["labwc-locachy", "swaybg"],
                                                "view": ["libmupdf", "libarchive", "libraw", "chmlib"],
                                                "audio": ["pipewire", "wiremix"]})
        self.assertEqual(self.run_declare(), [("module view", ["chmlib"])])

    def test_no_manifest_or_not_a_regular_file(self):
        self.assertEqual(self.run_declare(), [])
        self.assertFalse(self.record.exists())
        other = self.root / "other"
        other.write_text("base\n")
        self.manifest.symlink_to(other)
        with self.assertRaises(ValueError):
            self.run_declare()
        self.assertEqual(other.read_text(), "base\n")

    def test_shipped_definitions_declare_what_came_after_08d2a66(self):
        """The real source on a system installed at 08d2a66 with desktop and view."""
        base = json.loads((SOURCE / declare.BASELINE).read_text())
        self.assertEqual(base["commit"], "08d2a66")
        kept = [p for p in base["base"] if not p.startswith("linux-cachyos")] + ["linux-lts"]
        self.manifest.write_text("".join(p + "\n" for p in kept + base["modules"]["desktop"] +
                                         base["modules"]["view"]))
        with contextlib.redirect_stdout(io.StringIO()):
            groups = dict(declare.declare(SOURCE, self.manifest, self.record))
        self.assertEqual(groups.get("module desktop"), ["xorg-xwayland", "xorg-xhost"])
        self.assertEqual(groups.get("module view"), ["libraw", "chmlib"])
        self.assertNotIn("module audio", groups)
        self.assertFalse(any(p.startswith("linux-cachyos") for p in self.leaves()))
        current = declare.definitions(SOURCE)
        self.assertEqual(groups.get("base", []), [p for p in current["base"] if p not in base["base"]])


if __name__ == "__main__":
    unittest.main()
