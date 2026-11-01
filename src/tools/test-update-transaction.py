#!/usr/bin/env python3
"""Prepared-update ordering, input pinning and real-lock discipline without
pacman, mount, network or host changes. Every path is under SCRATCH_PARENT.
  python3 -B tools/test-update-transaction.py SCRATCH_PARENT
"""
import contextlib
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
LINK = Path(__file__).resolve().parents[1] / "link"


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


tx = load("transaction_test_module", LINK / "locachy_transaction.py")
pkg = load("transaction_pkg_module", LINK / "locachy-pkg")
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class UpdateTransaction(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="update-transaction-", dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sync = self.root / "check/sync"
        self.sync.mkdir(parents=True)
        (self.sync / "extra.db").write_bytes(b"fixed repository database")
        self.recipe = self.root / "source/pkgbuilds/example"
        self.recipe.mkdir(parents=True)
        self.marker = self.root / "recipe-was-executed"
        (self.recipe / "PKGBUILD").write_text(f"touch '{self.marker}'\npkgname=example\n")
        (self.recipe / "guard.patch").write_bytes(b"local patch")
        self.config = self.root / "pacman.conf"
        mirror = self.root / "mirrorlist"
        mirror.write_text("Server = https://mirror.example/$repo/os/$arch\n")
        self.config.write_text(f"[options]\nArchitecture = x86_64\n[extra]\nInclude = {mirror}\n")
        self.records = self.root / "records"
        self.host = "host-before"
        self.reference = tx.snapshot(self.records, self.sync, self.config, self.host,
                                     {"example": {"baseline_dir": str(self.recipe), "recipe_dir": str(self.recipe)}})
        self.directory, self.data = tx.load_snapshot(self.reference, self.host)
        self.db = self.root / "real-db"
        (self.db / "local").mkdir(parents=True)
        (self.db / "local/unchanged").write_text("old")

    def result(self, name="example"):
        prepared = self.directory / "prepared" / name
        prepared.mkdir(parents=True, exist_ok=True)
        artifact = prepared / f"{name}-2-1-x86_64.pkg.tar.zst"
        artifact.write_bytes(b"tested candidate archive")
        (prepared / "target-base-packages.txt").write_text("runtime 2-1\n")
        (prepared / "target-packages.txt").write_text(f"runtime 2-1\n{name} 2-1\n")
        value = {"name": name, "artifact": str(artifact), "sha256": tx.file_hash(artifact),
                 "version": "2-1", "source_host_sha256": self.host,
                 "recipe_sha256": self.data["recipes"][name]["recipe_sha256"],
                 "target_packages_sha256": tx.file_hash(prepared / "target-packages.txt"),
                 "target_base_sha256": tx.file_hash(prepared / "target-base-packages.txt"), "tests": {"passed": True},
                 "record": {"recipe": "effective-hash", "host": "target-hash"}}
        tx.write_json(prepared / "result.json", value)
        return value

    def test_snapshot_pins_database_config_and_recipe_without_shell(self):
        text = (self.directory / "pacman.conf").read_text()
        self.assertIn("https://mirror.example", text)
        self.assertNotIn("Include =", text)
        self.assertFalse(self.marker.exists())
        (self.recipe / "guard.patch").write_text("changed")
        with self.assertRaisesRegex(tx.TransactionError, "patch inputs changed"):
            tx.load_snapshot(self.reference, self.host)

    def test_changed_host_or_repository_is_rejected(self):
        with self.assertRaisesRegex(tx.TransactionError, "host packages changed"):
            tx.load_snapshot(self.reference, "new-host")
        (self.directory / "sync/extra.db").write_text("different")
        with self.assertRaisesRegex(tx.TransactionError, "repository snapshot changed"):
            tx.load_snapshot(self.reference, self.host)

    def test_failed_build_occurs_before_any_package_transaction(self):
        calls = []
        def run(command):
            calls.append(command)
            return subprocess.CompletedProcess(command, 7)
        with self.assertRaises(tx.TransactionError):
            tx.prepare(self.reference, "/candidate/locachy-build", lambda: self.host, run)
        self.assertEqual(calls, [["/candidate/locachy-build", "--prepare", str(self.directory), "--local", "example"]])
        self.assertEqual((self.db / "local/unchanged").read_text(), "old")
        self.assertFalse((self.db / "db.lck").exists())

    def test_gate_failure_and_modified_artifact_fail_closed(self):
        result = self.result()
        result["tests"]["passed"] = False
        tx.write_json(self.directory / "prepared/example/result.json", result)
        with self.assertRaisesRegex(tx.TransactionError, "tests did not pass"):
            tx.prepared_result(self.directory, "example", self.data)
        result["tests"]["passed"] = True
        tx.write_json(self.directory / "prepared/example/result.json", result)
        Path(result["artifact"]).write_bytes(b"changed after test")
        with self.assertRaisesRegex(tx.TransactionError, "artifact hash changed"):
            tx.prepared_result(self.directory, "example", self.data)

    def test_prepare_validates_exact_candidate_and_has_no_host_install(self):
        calls = []
        def run(command):
            calls.append(command)
            self.result()
            return subprocess.CompletedProcess(command, 0)
        directory, data, results = tx.prepare(self.reference, "builder", lambda: self.host, run)
        self.assertEqual(directory, self.directory)
        self.assertEqual(set(results), {"example"})
        self.assertEqual(calls, [["builder", "--prepare", str(self.directory), "--local", "example"],
                                 ["builder", "--validate-prepared", str(self.directory)]])
        self.assertFalse(any("pacman" in c for c in calls))

    def test_preview_notice_is_shown_but_not_parsed_as_package(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            manifest = tx.preview_manifest("Update notice: keep this visible\nLOCACHY-PACKAGE package 1-1\n")
        self.assertEqual(manifest, "package 1-1\n")
        self.assertIn("keep this visible", output.getvalue())
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(tx.TransactionError):
            tx.preview_manifest("Update notice: no package records\n")
        with self.assertRaises(tx.TransactionError):
            tx.preview_manifest("LOCACHY-PACKAGE a 1\nLOCACHY-PACKAGE a 2\n")

    def test_all_contracts_bind_same_root_and_entire_artifact_set(self):
        first = self.result()
        self.data["recipes"]["other"] = dict(self.data["recipes"]["example"])
        second = self.result("other")
        for item in (first, second):
            item["target_packages_sha256"] = "a" * 64
            item["target_base_sha256"] = "b" * 64
        results = {"example": first, "other": second}
        with self.assertRaisesRegex(tx.TransactionError, "complete candidate artifact set"):
            tx.verify_combined_results(results)
        artifacts = {name: item["sha256"] for name, item in results.items()}
        for item in results.values():
            item["tests"]["artifacts"] = dict(artifacts)
        tx.verify_combined_results(results)
        second["sha256"] = "c" * 64
        with self.assertRaisesRegex(tx.TransactionError, "complete candidate artifact set"):
            tx.verify_combined_results(results)
        second["sha256"] = artifacts["other"]
        second["target_packages_sha256"] = "d" * 64
        with self.assertRaisesRegex(tx.TransactionError, "different target package sets"):
            tx.verify_combined_results(results)

    def test_failed_combined_contract_does_not_leave_old_success_result(self):
        self.result()
        calls = []
        def run(command):
            calls.append(command)
            return subprocess.CompletedProcess(command, 7 if "--validate-prepared" in command else 0)
        with self.assertRaises(tx.TransactionError):
            tx.prepare(self.reference, "builder", lambda: self.host, run)
        self.assertFalse((self.directory / "prepared/example/result.json").exists())
        self.assertEqual(calls[-1], ["builder", "--validate-prepared", str(self.directory)])

    def test_one_fixed_snapshot_transaction_holds_actual_lock(self):
        result = self.result()
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            if command[0] == "repo-add":
                Path(command[1]).write_bytes(b"candidate repository")
            else:
                self.assertEqual(command[:4], ["unshare", "--mount", "--propagation", "private"])
                self.assertIn('mount --rbind "${2%/local}" "${1%/local}"', command[6])
                real, private_local = command[8:10]
                self.assertEqual(Path(real), (self.db / "local").resolve())
                command = command[10:]
                self.assertEqual(command[0], "pacman")
                self.assertTrue((self.db / "db.lck").is_file())
                with self.assertRaises(FileExistsError):
                    os.open(self.db / "db.lck", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                self.assertNotIn("--dbpath", command)
                private = Path(private_local).parent
                self.assertEqual(Path(private_local), private / "local")
                self.assertTrue((private / "local").is_dir() and not (private / "local").is_symlink())
                self.assertEqual((private / "sync/extra.db").read_bytes(), b"fixed repository database")
                config = Path(command[command.index("--config") + 1]).read_text()
                self.assertLess(config.index("[locachy-validated]"), config.index("[extra]"))
                self.assertIn("file://", config)
                self.assertTrue("-Su" in command or "-Sup" in command)
                self.assertNotIn("-Syu", command)
                self.assertNotIn("-y", command)
                self.assertIn("locachy-validated/example=2-1", command)
                self.assertEqual(command[command.index("--assume-installed") + 1], "initramfs")
                if "-Sup" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="Update notice: fixture warning\n"
                                                       "LOCACHY-PACKAGE runtime 2-1\nLOCACHY-PACKAGE example 2-1\n")
                for name in ("runtime-2-1", "example-2-1"):
                    (self.db / "local" / name).mkdir()
                    (self.db / "local" / name / "desc").write_text("entry")
                    (self.db / "local" / name / "files").write_text("entry")
            return subprocess.CompletedProcess(command, 0)
        (self.db / "sync").mkdir()
        (self.db / "sync/extra.db").write_bytes(b"older database")
        (self.db / "sync/other.db").write_bytes(b"not in the snapshot")
        self.assertTrue(tx.install(self.reference, self.directory, self.data, {"example": result},
                                   lambda: self.host, str(self.db), run))
        self.assertEqual(len(calls), 3)
        self.assertFalse((self.db / "db.lck").exists())
        self.assertEqual((self.db / "local/unchanged").read_text(), "old")
        # The host's databases are now the snapshot the system matches.
        self.assertEqual((self.db / "sync/extra.db").read_bytes(), b"fixed repository database")
        self.assertEqual((self.db / "sync/other.db").read_bytes(), b"not in the snapshot")
        self.assertFalse(list((self.db / "sync").glob("*.locachy-new")))

    def test_unrecorded_or_leftover_entries_fail_the_install(self):
        local = self.db / "local"
        (local / "example-2-1").mkdir()
        with self.assertRaisesRegex(tx.TransactionError, "did not record example 2-1"):
            tx.verify_recorded("example 2-1\n", local)
        (local / "example-2-1/desc").write_text("entry")
        (local / "example-2-1/files").write_text("entry")
        (local / "example-1-1").mkdir()
        (local / "example-extra-1-1").mkdir()
        with self.assertRaisesRegex(tx.TransactionError, "old local database entries remain: example-1-1"):
            tx.verify_recorded("example 2-1\n", local)
        (local / "example-1-1").rmdir()
        tx.verify_recorded("example 2-1\n", local)

    def test_existing_real_lock_is_never_removed(self):
        result = self.result()
        (self.db / "db.lck").write_text("another pacman")
        calls = []
        def run(command):
            calls.append(command)
            if command[0] == "repo-add": Path(command[1]).write_bytes(b"repo")
            return subprocess.CompletedProcess(command, 0)
        with self.assertRaisesRegex(tx.TransactionError, "database is locked"):
            tx.install(self.reference, self.directory, self.data, {"example": result}, lambda: self.host,
                       str(self.db), run)
        self.assertEqual((self.db / "db.lck").read_text(), "another pacman")
        self.assertFalse(any(c[0] == "pacman" for c in calls))

    def test_package_failure_does_not_publish_records(self):
        result = self.result()
        def run(command, **kwargs):
            if command[0] == "repo-add":
                Path(command[1]).write_bytes(b"repo")
                return subprocess.CompletedProcess(command, 0)
            return subprocess.CompletedProcess(command, 1)
        (self.db / "sync").mkdir()
        (self.db / "sync/extra.db").write_bytes(b"older database")
        with self.assertRaises(tx.TransactionError):
            tx.install(self.reference, self.directory, self.data, {"example": result}, lambda: self.host,
                       str(self.db), run)
        self.assertFalse((self.records / "example.json").exists())
        self.assertFalse((self.db / "db.lck").exists())
        self.assertEqual((self.db / "sync/extra.db").read_bytes(), b"older database")

    def test_install_rejects_dependency_versions_not_in_test_root(self):
        result = self.result()
        with self.assertRaisesRegex(tx.TransactionError, "was not tested"):
            tx.verify_install_versions("runtime 3-1\nexample 2-1\n", self.directory, {"example": result})
        target = self.directory / "prepared/example/target-packages.txt"
        target.write_text("runtime 9-1\nexample 2-1\n")
        with self.assertRaisesRegex(tx.TransactionError, "dependency manifest changed"):
            tx.prepared_result(self.directory, "example", self.data)

    def test_trusted_gate_sources_are_pinned_before_build(self):
        source = self.root / "trusted-source"
        source.mkdir()
        for name in tx.TEST_PATHS:
            if name in ("rc.conf", "packages"):
                (source / name).write_text("fixture\n")
            else:
                (source / name).mkdir()
                (source / name / "input").write_text("trusted\n")
        ref = tx.snapshot(self.records, self.sync, self.config, self.host,
                          {"example": {"baseline_dir": str(self.recipe)}}, test_source=source)
        tx.load_snapshot(ref, self.host)
        (source / "tools/input").write_text("changed test\n")
        with self.assertRaisesRegex(tx.TransactionError, "trusted test sources changed"):
            tx.load_snapshot(ref, self.host)

    def test_declared_packages_are_pinned_tested_and_in_the_same_transaction(self):
        with self.assertRaisesRegex(tx.TransactionError, "invalid declared package name"):
            tx.snapshot(self.records, self.sync, self.config, self.host,
                        {"example": {"baseline_dir": str(self.recipe)}}, missing=["-rf"])
        self.reference = tx.snapshot(self.records, self.sync, self.config, self.host,
                                     {"example": {"baseline_dir": str(self.recipe), "recipe_dir": str(self.recipe)}},
                                     missing=["libraw", "chmlib", "libraw"])
        self.directory, self.data = tx.load_snapshot(self.reference, self.host)
        self.assertEqual(self.data["missing"], ["chmlib", "libraw"])
        result = self.result()
        # A library the contracts' root did not have is refused at install.
        with self.assertRaisesRegex(tx.TransactionError, "libraw=0.22-1 was not tested"):
            tx.verify_install_versions("libraw 0.22-1\nexample 2-1\n", self.directory, {"example": result})
        target = self.directory / "prepared/example/target-packages.txt"
        target.write_text("runtime 2-1\nchmlib 0.40-1\nlibraw 0.22-1\nexample 2-1\n")
        result["target_packages_sha256"] = tx.file_hash(target)
        tx.write_json(self.directory / "prepared/example/result.json", result)
        commands = []
        def run(command, **kwargs):
            if command[0] == "repo-add":
                Path(command[1]).write_bytes(b"candidate repository")
                return subprocess.CompletedProcess(command, 0)
            command = command[10:]
            commands.append(command)
            if "-Sup" in command:
                return subprocess.CompletedProcess(command, 0, stdout="LOCACHY-PACKAGE chmlib 0.40-1\n"
                                                   "LOCACHY-PACKAGE libraw 0.22-1\nLOCACHY-PACKAGE example 2-1\n")
            for name in ("chmlib-0.40-1", "libraw-0.22-1", "example-2-1"):
                (self.db / "local" / name).mkdir()
                (self.db / "local" / name / "desc").write_text("entry")
                (self.db / "local" / name / "files").write_text("entry")
            return subprocess.CompletedProcess(command, 0)
        self.assertTrue(tx.install(self.reference, self.directory, self.data, {"example": result},
                                   lambda: self.host, str(self.db), run))
        for command in commands:
            self.assertEqual(command[-3:], ["locachy-validated/example=2-1", "chmlib", "libraw"])

    def test_host_failure_stops_dependent_targets_and_preserves_plan(self):
        rows = [("host", "local", "example", "1", "2"), ("locachy", "rebuild", "programs", "", ""),
                ("demo", "rebuild", "host changes", "", "")]
        calls = []
        with mock.patch.object(pkg, "load_plan", return_value=(rows, "checked")), \
                mock.patch.object(pkg, "containers", return_value=["demo"]), \
                mock.patch.object(pkg, "apply_target", side_effect=lambda target, _: calls.append(target) or False), \
                mock.patch.object(pkg, "plan", side_effect=AssertionError("must not overwrite failed plan")), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply([]), 1)
        self.assertEqual(calls, ["host"])


if __name__ == "__main__":
    unittest.main()
