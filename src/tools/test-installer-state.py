#!/usr/bin/env python3
"""Installer content fingerprints and retry boundaries; no root or package tools.
  python3 -B tools/test-installer-state.py [SCRATCH_PARENT]
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
spec = importlib.util.spec_from_file_location("installer_state", SOURCE / "link/locachy_install_state.py")
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


class State(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="installer-state-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.stages = state.Stages(self.directory / "state")

    def test_success_is_reused_but_failed_and_changed_work_runs_again(self):
        calls = []
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(self.stages.run("packages", {"manifest": "one"}, lambda: calls.append(1)))
            self.assertFalse(self.stages.run("packages", {"manifest": "one"}, lambda: calls.append(2)))
            with self.assertRaisesRegex(RuntimeError, "fixture failure"):
                self.stages.run("packages", {"manifest": "two"},
                                lambda: (_ for _ in ()).throw(RuntimeError("fixture failure")))
            self.assertNotIn("packages", state.Stages(self.directory / "state").stages)
            self.assertTrue(self.stages.run("packages", {"manifest": "two"}, lambda: calls.append(3)))
        self.assertEqual(calls, [1, 3])
        self.assertEqual((self.directory / "state/stages.json").stat().st_mode & 0o777, 0o600)

    def test_changed_software_stage_invalidates_downstream_not_account_or_hardware(self):
        for name in state.DEPENDENTS:
            self.stages.complete(name, name)
        self.stages.begin("packages")
        for name in ("system", "configuration", "boot", "publish"):
            self.assertNotIn(name, self.stages.stages)
        for name in ("account", "tpm", "secure-boot-keys", "secure-boot-enroll"):
            self.assertIn(name, self.stages.stages)

    def test_old_record_has_no_success_and_target_disk_identity_is_not_in_this_journal(self):
        self.assertTrue(self.stages.needed("sources", "anything"))
        self.stages.complete("account", "account")
        data = json.loads((self.directory / "state/stages.json").read_text())
        self.assertEqual(data["schema"], state.SCHEMA)
        self.assertNotIn("disk", data)
        self.assertNotIn("password", data)

    def source(self):
        source = self.directory / "source"
        for name in ("packages", "link/modules.json", "base/a.c", "progs/installer.c", "aur-patches/example.sed"):
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        return source

    def test_content_not_timestamp_or_git_commit_controls_fingerprints(self):
        source = self.source()
        before = state.files_digest(source, state.SOURCE_INPUTS)
        os.utime(source / "base/a.c", (1, 1))
        self.assertEqual(before, state.files_digest(source, state.SOURCE_INPUTS))
        (source / ".git").mkdir()
        (source / ".git/HEAD").write_text("another commit")
        self.assertEqual(before, state.files_digest(source, state.SOURCE_INPUTS))
        (source / "base/a.c").write_text("changed")
        self.assertNotEqual(before, state.files_digest(source, state.SOURCE_INPUTS))

    def test_program_change_keeps_package_fingerprint_but_patch_change_invalidates_it(self):
        source = self.source()
        target = self.directory / "target"
        before = state.build_input(source, target, "packages", "x86-64-v2", "ivybridge")
        (source / "progs/installer.c").write_text("changed UI")
        self.assertEqual(before, state.build_input(source, target, "packages", "x86-64-v2", "ivybridge"))
        (source / "aur-patches/example.sed").write_text("changed patch")
        self.assertNotEqual(before, state.build_input(source, target, "packages", "x86-64-v2", "ivybridge"))

    def test_installed_abi_invalidates_program_build(self):
        source = self.source()
        target = self.directory / "target"
        before = state.build_input(source, target, "system", "x86-64-v2", "ivybridge")
        desc = target / "var/lib/pacman/local/glibc-2/desc"
        desc.parent.mkdir(parents=True)
        desc.write_text("%NAME%\nglibc\n\n%VERSION%\n2\n")
        self.assertNotEqual(before, state.build_input(source, target, "system", "x86-64-v2", "ivybridge"))

    def test_changed_installed_aur_patch_rebuilds_same_version_and_failed_record_is_not_advanced(self):
        source = self.source()
        target = self.directory / "target"
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(state.subprocess, "run", run), contextlib.redirect_stdout(io.StringIO()):
            state.rebuild_patches(source, target)
            self.assertEqual(calls[-1], ["chroot", str(target), "/usr/local/bin/locachy-build", "-i", "-r", "example"])
            calls.clear()
            state.rebuild_patches(source, target)
            self.assertEqual(calls, [])
            (source / "aur-patches/example.sed").write_text("fix2")
            before = (target / ".locachy-installer/aur-patches.json").read_bytes()
            with mock.patch.object(state.subprocess, "run", side_effect=[subprocess.CompletedProcess([], 0),
                                      subprocess.CalledProcessError(1, "build")]), self.assertRaises(subprocess.CalledProcessError):
                state.rebuild_patches(source, target)
            self.assertEqual((target / ".locachy-installer/aur-patches.json").read_bytes(), before)

    def test_command_reuse_and_corrupt_state_have_distinct_exit_codes(self):
        tool = str(SOURCE / "link/locachy_install_state.py")
        directory = self.directory / "state"
        self.stages.complete("packages", "known")
        command = [sys.executable, "-I", tool, "needed", str(directory), "packages", "known"]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 10)
        (directory / "stages.json").write_text("{not json}")
        self.assertNotIn(subprocess.run(command, capture_output=True).returncode, (0, 10))

    def test_symlinked_state_or_source_input_is_refused(self):
        source = self.source()
        (source / "outside").mkdir()
        (source / "base/a.c").unlink()
        (source / "base/a.c").symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(ValueError):
            state.files_digest(source, state.SOURCE_INPUTS)
        path = self.directory / "stages.json"
        path.symlink_to(self.directory / "elsewhere")
        with self.assertRaises(ValueError):
            state.write_json(path, {})


if __name__ == "__main__":
    unittest.main()
