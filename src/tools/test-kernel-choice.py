#!/usr/bin/env python3
"""Kernel/header choices use the host manifest; no package manager is run."""
import contextlib
import importlib.machinery
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("kernel_package_tool", str(SOURCE / "link/locachy-pkg"))
spec = importlib.util.spec_from_loader(loader.name, loader)
pkg = importlib.util.module_from_spec(spec)
loader.exec_module(pkg)
KERNEL_PACKAGES = pkg.kernel_packages
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class KernelChoice(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="kernel-choice-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "packages"
        self.manifest.write_text("# user leaves\nbase\nlinux-cachyos-bore\n")
        self.rc = self.root / "rc.conf"
        self.rc.write_text('boot_verbose="1"\n')
        self.calls = []
        for name, value in (("MANIFEST", str(self.manifest)), ("RC_CONF", str(self.rc)),
                            ("cmd_sync", lambda: self.calls.append("sync") or 0),
                            ("kernel_packages", lambda: ["linux", "linux-lts", "linux-cachyos-bore"]),
                            ("run", lambda cmd: self.calls.append(cmd) or True)):
            patch = mock.patch.object(pkg, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        # load_manifest's default argument is bound at definition time.
        load = pkg.load_manifest
        patch = mock.patch.object(pkg, "load_manifest", lambda: load(str(self.manifest)))
        patch.start()
        self.addCleanup(patch.stop)

    def run_choice(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return pkg.cmd_kernel(list(args))

    def leaves(self):
        return set(pkg.load_manifest())

    def test_headers_are_not_mandatory_default(self):
        self.assertNotIn("linux-cachyos-bore-headers", self.leaves())
        shipped = (SOURCE / "packages").read_text().splitlines()
        self.assertNotIn("linux-cachyos-bore-headers", shipped)

    def test_on_and_off_are_declared_and_reconciled(self):
        self.assertEqual(self.run_choice("headers", "on"), 0)
        self.assertIn("linux-cachyos-bore-headers", self.leaves())
        self.assertEqual(self.calls, ["sync"])
        self.assertEqual(self.run_choice("headers", "on"), 0)
        self.assertEqual(self.manifest.read_text().count("linux-cachyos-bore-headers"), 1)
        self.assertEqual(self.run_choice("headers", "off"), 0)
        self.assertNotIn("linux-cachyos-bore-headers", self.leaves())
        self.assertIn("linux-cachyos-bore", self.leaves())
        self.assertIn("# user leaves", self.manifest.read_text())

    def test_switch_preserves_headers_off(self):
        self.assertEqual(self.run_choice("select", "linux-lts"), 0)
        self.assertIn("linux-lts", self.leaves())
        self.assertNotIn("linux-lts-headers", self.leaves())
        self.assertIn("linux-cachyos-bore", self.leaves())
        self.assertEqual(pkg.selected_kernel(), "linux-lts")
        self.assertIn('boot_verbose="1"', self.rc.read_text())
        self.assertEqual(self.calls, ["sync", ["/usr/local/bin/locachy-boot"]])

    def test_switch_preserves_headers_on(self):
        self.assertEqual(self.run_choice("headers", "on"), 0)
        self.assertEqual(self.run_choice("select", "linux-lts"), 0)
        self.assertIn("linux-lts-headers", self.leaves())
        self.assertIn("linux-cachyos-bore-headers", self.leaves())
        self.assertEqual(self.run_choice("headers", "off"), 0)
        self.assertNotIn("linux-lts-headers", self.leaves())
        self.assertIn("linux-cachyos-bore-headers", self.leaves())

    def test_returning_to_an_older_kernel_preserves_off(self):
        self.assertEqual(self.run_choice("headers", "on"), 0)
        self.assertEqual(self.run_choice("select", "linux-lts"), 0)
        self.assertEqual(self.run_choice("headers", "off"), 0)
        self.assertEqual(self.run_choice("select", "linux-cachyos-bore"), 0)
        self.assertNotIn("linux-cachyos-bore-headers", self.leaves())

    def test_missing_repository_headers_do_not_become_aur_builds(self):
        before = self.manifest.read_text()
        with mock.patch.object(pkg, "kernel_packages", return_value=[]):
            self.assertEqual(self.run_choice("headers", "on"), 1)
        self.assertEqual(self.manifest.read_text(), before)
        self.assertEqual(self.calls, [])

    def test_failed_install_keeps_boot_selection_and_intent(self):
        with mock.patch.object(pkg, "cmd_sync", return_value=1):
            self.assertEqual(self.run_choice("select", "linux-lts"), 1)
        self.assertEqual(pkg.selected_kernel(), "linux-cachyos-bore")
        self.assertNotIn(["/usr/local/bin/locachy-boot"], self.calls)
        self.assertIn("linux-lts", self.leaves())

    def test_failed_image_restores_previous_selection(self):
        with mock.patch.object(pkg, "run", return_value=False):
            self.assertEqual(self.run_choice("select", "linux-lts"), 1)
        self.assertEqual(pkg.selected_kernel(), "linux-cachyos-bore")

    def test_boot_exec_failure_restores_previous_selection(self):
        with mock.patch.object(pkg, "run", side_effect=OSError("not executable")):
            self.assertEqual(self.run_choice("select", "linux-lts"), 1)
        self.assertEqual(pkg.selected_kernel(), "linux-cachyos-bore")

    def test_custom_arguments_and_injection_are_rejected(self):
        before = self.manifest.read_text()
        for args in (("headers", "maybe"), ("select", "linux;rm"), ("select", "linux-headers"), ("select", "--help")):
            self.assertEqual(self.run_choice(*args), 1)
        self.assertEqual(self.manifest.read_text(), before)
        self.assertEqual(self.calls, [])

    def test_repository_pairs_exclude_module_addons(self):
        listing = "linux\nlinux-headers\nlinux-lts\nlinux-lts-headers\nlinux-cachyos-bore\nlinux-cachyos-bore-headers\nlinux-cachyos-bore-nvidia-open\nlinux-api-headers\n"
        with mock.patch.object(pkg.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, listing, "")):
            self.assertEqual(KERNEL_PACKAGES(), ["linux", "linux-cachyos-bore", "linux-lts"])


if __name__ == "__main__":
    unittest.main()
