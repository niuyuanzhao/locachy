#!/usr/bin/env python3
"""Prepared-update ordering, input pinning and real-lock discipline without
pacman, mount, network or host changes. Every path is under SCRATCH_PARENT.
  python3 -B tools/test-update-transaction.py SCRATCH_PARENT
"""
import contextlib
import hashlib
import importlib.machinery
import importlib.util
import inspect
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
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
HAVE_PACMAN = all(shutil.which(tool) for tool in ("pacman", "pacman-conf", "bsdtar"))


def sync_db(path, packages):
    """packages: [(name, version)] as a repository database pacman reads."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for name, version in packages:
            data = (f"%FILENAME%\n{name}-{version}-x86_64.pkg.tar.zst\n\n%NAME%\n{name}\n\n%BASE%\n{name}\n\n"
                    f"%VERSION%\n{version}\n\n%DESC%\nfixture\n\n%CSIZE%\n1024\n\n%ISIZE%\n4096\n\n"
                    f"%SHA256SUM%\n{'0' * 64}\n\n%ARCH%\nx86_64\n\n%BUILDDATE%\n1700000000\n\n"
                    "%PACKAGER%\nfixture\n\n").encode()
            info = tarfile.TarInfo(f"{name}-{version}/desc")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    Path(path).write_bytes(buffer.getvalue())


def local_db(path):
    """An empty local database pacman accepts."""
    Path(path).mkdir(parents=True, exist_ok=True)
    version = Path("/var/lib/pacman/local/ALPM_DB_VERSION")
    (Path(path) / "ALPM_DB_VERSION").write_text(version.read_text() if version.is_file() else "9\n")


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

    def test_results_valid_for_this_snapshot_are_reused_without_a_build(self):
        """A retried apply of the same plan: every check prepared_result makes
        still passes, so nothing is built or tested again. A result that no
        longer matches the snapshot is built and tested again."""
        self.result()
        calls = []
        def run(command):
            calls.append(command)
            self.result()
            return subprocess.CompletedProcess(command, 0)
        with contextlib.redirect_stdout(io.StringIO()):
            directory, data, results = tx.prepare(self.reference, "builder", lambda: self.host, run)
        self.assertEqual(calls, [])
        self.assertEqual(set(results), {"example"})
        stale = self.result()
        stale["recipe_sha256"] = "0" * 64
        tx.write_json(self.directory / "prepared/example/result.json", stale)
        with contextlib.redirect_stdout(io.StringIO()):
            tx.prepare(self.reference, "builder", lambda: self.host, run)
        self.assertEqual(calls, [["builder", "--prepare", str(self.directory), "--local", "example"],
                                 ["builder", "--validate-prepared", str(self.directory)]])

    def test_failed_combined_contract_does_not_leave_old_success_result(self):
        old = self.result()
        old["recipe_sha256"] = "0" * 64  # from another snapshot: not reusable, so built again
        tx.write_json(self.directory / "prepared/example/result.json", old)
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
        (self.db / "sync/extra.db.sig").write_bytes(b"signature of the older database")
        (self.db / "sync/other.db").write_bytes(b"not in the snapshot")
        record = self.root / "sync-published.json"
        self.assertTrue(tx.install(self.reference, self.directory, self.data, {"example": result},
                                   lambda: self.host, str(self.db), run, record=str(record)))
        self.assertEqual(len(calls), 3)
        self.assertFalse((self.db / "db.lck").exists())
        self.assertEqual((self.db / "local/unchanged").read_text(), "old")
        # The host's databases are now the snapshot the system matches. The
        # snapshot's extra.db has no signature (the stack rewrote it): the old
        # one, of another database, is gone.
        self.assertEqual((self.db / "sync/extra.db").read_bytes(), b"fixed repository database")
        self.assertFalse((self.db / "sync/extra.db.sig").exists())
        self.assertEqual((self.db / "sync/other.db").read_bytes(), b"not in the snapshot")
        self.assertFalse(list((self.db / "sync").glob("*.locachy-new")))
        # And recorded as published, for locachy-pacman-guard.
        self.assertEqual(json.loads(record.read_text()),
                         {"extra.db": tx.file_hash(self.db / "sync/extra.db"),
                          "other.db": tx.file_hash(self.db / "sync/other.db")})

    def test_publish_pairs_each_database_with_its_own_signature_only(self):
        snapshot, host = self.root / "snapshot-sync", self.root / "host-sync"
        snapshot.mkdir()
        host.mkdir()
        (snapshot / "core.db").write_bytes(b"core, unchanged by the stack")
        (snapshot / "core.db.sig").write_bytes(b"signature of core")
        (snapshot / "extra.db").write_bytes(b"extra, rewritten by the stack")
        os.utime(snapshot / "extra.db", (0, 0))  # as locachy_stack dates a rewritten one
        os.utime(snapshot / "core.db", (1700000000, 1700000000))  # the mirror's time
        for name in ("core", "extra"):
            (host / f"{name}.db").write_bytes(b"older")
            (host / f"{name}.db.sig").write_bytes(b"signature of the older")
        tx.publish_sync(snapshot, host)
        # The times go over too: a plain pacman -Sy then fetches the rewritten
        # database whole and finds the unchanged one up to date.
        self.assertEqual((host / "extra.db").stat().st_mtime, 0)
        self.assertEqual((host / "core.db").stat().st_mtime, 1700000000)
        self.assertEqual((host / "core.db").read_bytes(), b"core, unchanged by the stack")
        self.assertEqual((host / "core.db.sig").read_bytes(), b"signature of core")
        self.assertEqual((host / "extra.db").read_bytes(), b"extra, rewritten by the stack")
        self.assertFalse((host / "extra.db.sig").exists())
        self.assertEqual(sorted(p.name for p in host.iterdir()), ["core.db", "core.db.sig", "extra.db"])
        for path in host.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o644)

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
        record = self.root / "sync-published.json"
        with self.assertRaisesRegex(tx.TransactionError, "database is locked"):
            tx.install(self.reference, self.directory, self.data, {"example": result}, lambda: self.host,
                       str(self.db), run, record=str(record))
        self.assertEqual((self.db / "db.lck").read_text(), "another pacman")
        self.assertFalse(any(c[0] == "pacman" for c in calls))
        self.assertFalse(record.exists())

    def test_package_failure_does_not_publish_records(self):
        result = self.result()
        def run(command, **kwargs):
            if command[0] == "repo-add":
                Path(command[1]).write_bytes(b"repo")
                return subprocess.CompletedProcess(command, 0)
            return subprocess.CompletedProcess(command, 1)
        (self.db / "sync").mkdir()
        (self.db / "sync/extra.db").write_bytes(b"older database")
        record = self.root / "sync-published.json"
        with self.assertRaises(tx.TransactionError):
            tx.install(self.reference, self.directory, self.data, {"example": result}, lambda: self.host,
                       str(self.db), run, record=str(record))
        self.assertFalse((self.records / "example.json").exists())
        self.assertFalse((self.db / "db.lck").exists())
        self.assertEqual((self.db / "sync/extra.db").read_bytes(), b"older database")
        self.assertFalse(record.exists())  # nothing published, nothing recorded

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
                                   lambda: self.host, str(self.db), run,
                                   record=str(self.root / "sync-published.json")))
        for command in commands:
            self.assertEqual(command[-3:], ["locachy-validated/example=2-1", "chmlib", "libraw"])

    def test_host_failure_stops_dependent_targets_and_preserves_plan(self):
        rows = [("host", "local", "example", "1", "2"), ("locachy", "rebuild", "programs", "", ""),
                ("demo", "rebuild", "host changes", "", "")]
        calls = []
        with mock.patch.object(pkg, "load_plan", return_value=(rows, "checked")), \
                mock.patch.object(pkg, "validate_plan_inputs", lambda: None), \
                mock.patch.object(pkg, "containers", return_value=["demo"]), \
                mock.patch.object(pkg, "apply_target", side_effect=lambda target, _: calls.append(target) or False), \
                mock.patch.object(pkg, "plan", side_effect=AssertionError("must not overwrite failed plan")), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply([]), 1)
        self.assertEqual(calls, ["host"])

    KEY = "5A618CE840883942BAF1334F009DE379FD9B7B90"
    UPSTREAM = "https://gitlab.torproject.org/tpo/anti-censorship/pluggable-transports/lyrebird"

    def apply_with_keys(self, rows, check_status, targets=(), build_status=0, tracked=False, source_ok=False,
                        after_source=False, mismatch=None):
        """cmd_apply over rows, every program it would start a double: the
        key check (locachy-build --check-keys) answers check_status, an AUR
        build build_status. seq: what was started and done, in order."""
        seq = []

        def run(command, **kwargs):
            command = list(command)
            seq.append(command)
            if command[:2] == [pkg.BUILD_TOOL, "--check-keys"]:
                out = (f"lyrebird-proxy\tlyrebird-proxy\t{self.KEY}\t{self.UPSTREAM}\t"
                       f"git+{self.UPSTREAM}.git#tag=lyrebird-0.6.2?signed\n" if check_status == 3 else "")
                return subprocess.CompletedProcess(command, check_status, stdout=out,
                                                   stderr="locachy-build: the AUR did not answer" if check_status == 1 else "")
            return subprocess.CompletedProcess(command, build_status if command[0] == pkg.BUILD_TOOL else 0)

        output = io.StringIO()
        installed = {"labwc-locachy": "1"} if tracked else {}
        patches = {"load_plan": mock.Mock(return_value=(rows, "checked")), "validate_plan_inputs": lambda: None,
                   "source_setup_pending": lambda: False, "containers": mock.Mock(return_value=[]),
                   "CONF_DIR": str(self.root / "no-containers"), "in_net": lambda command: command,
                   "publish_merged": lambda: seq.append("publish databases"), "transaction_reference": lambda: {},
                   "host_databases_mismatch": lambda: mismatch,
                   "install_prepared_host": lambda reference, recipes: seq.append("prepared transaction") or True,
                   "install_source_first": lambda commit: seq.append("source") or source_ok,
                   "apply_apps": lambda mine: seq.append("apps") or True,
                   "rebuild_locachy": lambda force=False: seq.append("make.sh") or True,
                   "baseline_recipes": lambda: {"labwc-locachy": "/fixture"} if tracked else {},
                   "tracked_recipes": lambda recipes: dict(recipes), "local_packages": lambda db: dict(installed),
                   "local_recipes": lambda build=True: {}, "local_names": lambda recipes=None: set(),
                   "declared_missing": lambda host: [], "plan": lambda *a, **k: pkg.Plan(rows),
                   "settle_source_update": lambda rows: rows, "save_plan": lambda rows: seq.append("save plan")}
        with contextlib.ExitStack() as stack:
            for name, value in patches.items():
                stack.enter_context(mock.patch.object(pkg, name, value))
            stack.enter_context(mock.patch.object(pkg.subprocess, "run", run))
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(output))
            # The updated updater's execv ends the first phase: "exec".
            stack.enter_context(mock.patch.object(pkg.os, "execv", side_effect=RuntimeError("exec")))
            try:
                status = pkg.cmd_apply(list(targets), after_source)
            except RuntimeError as e:
                if str(e) != "exec":
                    raise
                status = "exec"
        return status, seq, output.getvalue()

    def test_a_plain_refresh_is_published_again_with_nothing_to_upgrade(self):
        """The VM gate's G5(c): after a plain pacman -Sy, with nothing to upgrade
        the plan has no host rows. The apply must still publish the checked
        databases (the only thing that restores the record), and run no pacman
        transaction; a clean host publishes nothing."""
        rows = [("apps", "note", "catalog unchanged", "", "")]
        status, seq, out = self.apply_with_keys(rows, 0, mismatch="cachyos-v3.db is not the database LOCachy published")
        self.assertEqual(status, 0, out)
        self.assertEqual([s for s in seq if s != "save plan"], ["publish databases"])  # no transaction
        self.assertIn("publishing the checked databases again", out)
        status, seq, out = self.apply_with_keys(rows, 0, mismatch=None)
        self.assertNotIn("publish databases", seq)
        self.assertNotIn("publishing", out)

    def test_untrusted_aur_key_refuses_apply_before_any_package_transaction(self):
        """The VM gate's case: lyrebird-proxy's source key not trusted. Apply
        stops before the presets, the databases and pacman, says what to
        check and the command to trust it with, and is no "not done"."""
        rows = [("host", "repo", "openssl", "3.6.5-1", "4.1.0-1"),
                ("host", "aur", "lyrebird-proxy", "0.6.1-1", "0.6.2-1")]
        for tracked in (False, True):
            status, seq, out = self.apply_with_keys(rows, 3, tracked=tracked)
            self.assertEqual(status, pkg.KEYS_REFUSED, out)  # the jobs' "keys" state, not a failure
            self.assertEqual(seq, [[pkg.BUILD_TOOL, "--check-keys", "lyrebird-proxy"]])  # nothing else at all
            self.assertFalse(any(isinstance(c, list) and ("-Su" in c or "-U" in c or c[0] == "pacman") for c in seq))
            for text in (f"Source key to trust: lyrebird-proxy {self.KEY}", "package:      lyrebird-proxy",
                         f"fingerprint:  {self.KEY}", "validpgpkeys of the AUR recipe lyrebird-proxy",
                         "https://aur.archlinux.org/cgit/aur.git/tree/.SRCINFO?h=lyrebird-proxy",
                         f"git+{self.UPSTREAM}.git#tag=lyrebird-0.6.2?signed", f"{self.UPSTREAM} (its upstream project)",
                         f"then, root:   locachy-build --trust-key {self.KEY}",
                         f"Trust them: locachy-build --trust-key {self.KEY}",
                         "===> refused before any change: host",
                         "Nothing was changed: no package, repository database, package record or plan.",
                         "Trust the keys, then apply again."):
                self.assertIn(text, out)
            self.assertNotIn("not done", out)
        # Keys that could not be looked up (the AUR did not answer): refused
        # as well, before anything; never taken as trusted.
        status, seq, out = self.apply_with_keys(rows, 1)
        self.assertEqual(status, 1)
        self.assertEqual(seq, [[pkg.BUILD_TOOL, "--check-keys", "lyrebird-proxy"]])
        self.assertIn("could not be checked: locachy-build: the AUR did not answer", out)
        self.assertIn("===> refused before any change: host", out)
        self.assertNotIn("not done", out)
        # Only the host's AUR packages are looked up: a container's are not.
        status, seq, out = self.apply_with_keys([("demo", "aur", "lyrebird-proxy", "1", "2")], 3, targets=["host"])
        self.assertEqual((status, seq), (0, ["save plan"]))

    def test_a_locachy_source_update_checks_keys_only_after_its_source_is_installed(self):
        """An Apply that also installs a LOCachy source commit installs it first
        (by design); the installed locachy-build may predate --check-keys, so
        the first phase asks it nothing. The updated updater checks the keys
        before any host target and refuses there, the host still unchanged."""
        rows = [("host", "aur", "lyrebird-proxy", "0.6.1-1", "0.6.2-1"),
                ("locachy", "rebuild", "new source and recipes from branch main (make.sh --boot)", "404a02c", "a" * 40)]
        status, seq, out = self.apply_with_keys(rows, 3, source_ok=True)
        self.assertEqual((status, seq), ("exec", ["source"]), out)
        # The updated updater saves its own fresh check first (as it always
        # has), so its refusal does not claim the plan is unchanged.
        status, seq, out = self.apply_with_keys(rows, 3, after_source=True)
        self.assertEqual((status, seq),
                         (pkg.KEYS_REFUSED, ["save plan", [pkg.BUILD_TOOL, "--check-keys", "lyrebird-proxy"]]), out)
        self.assertIn("Nothing more was changed: the LOCachy source installed first stays; no package, "
                      "repository database or package record.", out)
        self.assertNotIn("record or plan", out)
        self.assertIn(f"Trust them: locachy-build --trust-key {self.KEY}", out)
        self.assertNotIn("not done", out)

    def test_trusted_aur_keys_let_the_transaction_go_on_and_the_build_still_checks(self):
        rows = [("host", "repo", "openssl", "3.6.5-1", "4.1.0-1"),
                ("host", "aur", "lyrebird-proxy", "0.6.1-1", "0.6.2-1")]
        check, build = [pkg.BUILD_TOOL, "--check-keys", "lyrebird-proxy"], [pkg.BUILD_TOOL, "-i", "lyrebird-proxy"]
        status, seq, out = self.apply_with_keys(rows, 0, targets=["host"])
        self.assertEqual(status, 0, out)
        self.assertEqual(seq, [check, "publish databases", ["pacman", "-Su", "--noconfirm"], build, "save plan"])
        self.assertIn("===> host: done", out)
        status, seq, out = self.apply_with_keys(rows, 0, targets=["host"], tracked=True)
        self.assertEqual(status, 0, out)
        self.assertEqual(seq, [check, "prepared transaction", build, "save plan"])
        # The second line of defence stays: a key the build stops for after
        # the transaction (trust withdrawn meanwhile) is a failure after it.
        status, seq, out = self.apply_with_keys(rows, 0, targets=["host"], tracked=True, build_status=3)
        self.assertEqual(status, 1, out)
        self.assertEqual(seq, [check, "prepared transaction", build])
        self.assertIn("===> host: not done", out)
        self.assertNotIn("refused before any change", out)

    def test_the_key_check_imports_fetches_and_trusts_nothing(self):
        """Every program check_aur_keys could start is a recording double: the
        one it starts is locachy-build --check-keys. Its own code names no
        GnuPG, keyserver, import or trust operation; --trust-key is only text
        shown to a person."""
        started = []

        def record(*args, **kwargs):
            command = list(args[0]) if args and isinstance(args[0], (list, tuple)) else list(args)
            started.append(command)
            return subprocess.CompletedProcess(command, 3, stdout=f"lyrebird-proxy\tlyrebird-proxy\t{self.KEY}\t-\t-\n",
                                               stderr="")
        rows = [("host", "aur", "lyrebird-proxy", "0.6.1-1", "0.6.2-1")]
        with mock.patch.object(pkg.subprocess, "run", record), mock.patch.object(pkg.subprocess, "Popen", record), \
                mock.patch.object(pkg.subprocess, "call", record), mock.patch.object(pkg.subprocess, "check_call", record), \
                mock.patch.object(pkg.subprocess, "check_output", record), mock.patch.object(pkg.os, "system", record), \
                mock.patch.object(pkg.os, "execv", record), mock.patch.object(pkg.os, "execvp", record), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(pkg.check_aur_keys(rows), pkg.KEYS_REFUSED)
        self.assertEqual(started, [[pkg.BUILD_TOOL, "--check-keys", "lyrebird-proxy"]])
        self.assertIn(f"Trust them: locachy-build --trust-key {self.KEY}", out.getvalue())
        self.assertNotIn("check it at:  -", out.getvalue())  # no upstream given: none shown
        source = inspect.getsource(pkg.check_aur_keys)
        for word in ("gpg", "--import", "--recv-keys", "--lsign", "--keyserver", "--edit-key", "--trust-model",
                     "LOCACHY_", "environ"):
            self.assertNotIn(word, source)
        self.assertEqual(set(re.findall(r"subprocess\.(\w+)", source)), {"run", "CompletedProcess"})
        self.assertIn('command = [BUILD_TOOL, "--check-keys"] + aur', source)
        trust_lines = [line for line in source.splitlines() if "--trust-key" in line]
        self.assertTrue(trust_lines)
        for line in trust_lines:  # --trust-key: in what is printed only
            self.assertIn("print(", line)
        # No switch around it: apply has no option or variable that skips the check.
        apply_source = inspect.getsource(pkg.cmd_apply)
        self.assertIn('if "host" in order and not source_first:\n        refused = check_aur_keys(rows, after_source)',
                      apply_source)

    def test_apply_refuses_a_plan_whose_inputs_changed(self):
        """The plan's binding: a source, manifest, rc.conf, host, pacman config
        or app configuration changed after Check means Apply refuses."""
        with tempfile.TemporaryDirectory(prefix="plan-binding-", dir=SCRATCH) as tmp:
            plan_file = os.path.join(tmp, "plan")
            inputs = {"source": "a", "manifest": "m"}
            with mock.patch.object(pkg, "PLAN_FILE", plan_file), \
                    mock.patch.object(pkg, "plan_inputs", lambda: self.fail("the plan's own binding is used")), \
                    mock.patch.object(pkg, "_pending_plan_lock", None):
                pkg.save_plan(pkg.Plan([("host", "repo", "base", "1", "2")], dict(inputs)))
            with mock.patch.object(pkg, "PLAN_FILE", plan_file), mock.patch.object(pkg, "plan_inputs", lambda: inputs):
                pkg.validate_plan_inputs()  # the same inputs: accepted
            with mock.patch.object(pkg, "PLAN_FILE", plan_file), \
                    mock.patch.object(pkg, "plan_inputs", lambda: {"source": "a", "manifest": "changed"}):
                with self.assertRaisesRegex(RuntimeError, "inputs changed"):
                    pkg.validate_plan_inputs()


class CheckedDatabases(unittest.TestCase):
    """locachy-pkg's view of the repositories: checkupdates' databases as the
    stack resolves them (MERGED_DB), and their publication to the host."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="checked-databases-", dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.checked = self.root / "checkupdates"
        self.merged = self.checked / "merged"
        self.db = self.root / "pacman"
        (self.db / "local").mkdir(parents=True)
        for name, value in (("CHECK_DB", str(self.checked)), ("MERGED_DB", str(self.merged)),
                            ("LOCAL_DB", str(self.db / "local"))):
            p = mock.patch.object(pkg, name, value)
            p.start()
            self.addCleanup(p.stop)

    def test_the_view_reads_the_real_local_database_through_a_link(self):
        (self.merged / "local").mkdir(parents=True)  # an empty directory: replaced by the link
        calls = []

        class Stack:
            def merge(source, target, repos):
                calls.append((source, target, repos))
                os.makedirs(target, exist_ok=True)

            def repositories(config):
                return ["core", "extra"]

        with mock.patch.object(pkg, "helper_module", lambda name: Stack):
            pkg.merge_checked()
            pkg.merge_checked()
        self.assertTrue(os.path.islink(self.merged / "local"))
        self.assertEqual(os.readlink(self.merged / "local"), str(self.db / "local"))
        self.assertEqual(calls, [(str(self.checked / "sync"), str(self.merged / "sync"), ["core", "extra"])] * 2)

    def test_publish_replaces_the_hosts_databases_under_the_lock_and_records_them(self):
        (self.merged / "sync").mkdir(parents=True)
        (self.merged / "sync/core.db").write_bytes(b"core, unchanged by the stack")
        (self.merged / "sync/core.db.sig").write_bytes(b"signature of core")
        (self.merged / "sync/extra.db").write_bytes(b"extra, rewritten by the stack")
        (self.db / "sync").mkdir()
        (self.db / "sync/extra.db").write_bytes(b"extra as a plain pacman -Sy fetched it")
        (self.db / "sync/extra.db.sig").write_bytes(b"signature of the raw extra")
        recorded = []
        with mock.patch.object(pkg, "helper_module", lambda name: tx), \
                mock.patch.object(tx, "record_published", lambda sync, record=None: recorded.append(
                    (sync, record, (self.db / "db.lck").exists()))):
            pkg.publish_merged()
        self.assertEqual((self.db / "sync/core.db.sig").read_bytes(), b"signature of core")
        self.assertEqual((self.db / "sync/extra.db").read_bytes(), b"extra, rewritten by the stack")
        self.assertFalse((self.db / "sync/extra.db.sig").exists())
        # Recorded, still under pacman's lock; the lock is gone afterwards.
        self.assertEqual(recorded, [(str(self.db / "sync"), None, True)])
        self.assertFalse((self.db / "db.lck").exists())

    def test_each_check_fetches_the_checked_databases_whole(self):
        # pacman keeps a database newer than the mirror's copy: every Check dates
        # the checked databases and their signatures at the epoch first.
        (self.checked / "sync").mkdir(parents=True)
        for name in ("multilib-x86-64-v2.db", "multilib-x86-64-v2.db.sig", "notes.txt"):
            (self.checked / "sync" / name).write_bytes(b"fetched earlier")
            os.utime(self.checked / "sync" / name, (1793925963, 1793925963))
        seen = {}

        def run(command, **kwargs):
            if command[0] == "env":  # checkupdates, the fetch
                seen.update({name: os.stat(self.checked / "sync" / name).st_mtime
                             for name in ("multilib-x86-64-v2.db", "multilib-x86-64-v2.db.sig", "notes.txt")})
            return subprocess.CompletedProcess(command, 2, stdout="", stderr="")

        with mock.patch.object(pkg.subprocess, "run", run), \
                mock.patch.object(pkg, "in_net", lambda command: command), \
                mock.patch.object(pkg, "merge_checked", lambda: None), \
                mock.patch.object(pkg, "ignored_packages", lambda: set()), \
                mock.patch.object(pkg, "local_packages", lambda db: {}):
            pkg.repo_updates()
        self.assertEqual(seen, {"multilib-x86-64-v2.db": 0.0, "multilib-x86-64-v2.db.sig": 0.0,
                                "notes.txt": 1793925963.0})

    @unittest.skipUnless(shutil.which("vercmp"), "pacman's vercmp is not installed here")
    def test_updates_are_the_views_newest_except_what_pacman_conf_holds_back(self):
        real_run = subprocess.run
        # The merged view: each name once, its winning layer's version (here core).
        listed = ("core openssl 4.1.0-1\ncore tor 0.4.9.16-1\ncore linux 7.4-1\n"
                  "cachyos-core-v3 zlib 1.3-1\n")

        def run(command, **kwargs):
            if command[0] == "vercmp":
                return real_run(command, **kwargs)
            if command[:2] == ["pacman", "-Sl"]:
                self.assertEqual(command, ["pacman", "-Sl", "--dbpath", str(self.merged)])
                return subprocess.CompletedProcess(command, 0, stdout=listed, stderr="")
            # checkupdates only fetches here; its own count (none) is not used.
            return subprocess.CompletedProcess(command, 2, stdout="", stderr="")

        installed = {"openssl": "3.6.5-1.1", "tor": "0.4.9.14-1.1", "linux": "7.3-2", "zlib": "1.3-1",
                     "bash": "5.3-1"}
        with mock.patch.object(pkg.subprocess, "run", run), \
                mock.patch.object(pkg, "in_net", lambda command: command), \
                mock.patch.object(pkg, "merge_checked", lambda: None), \
                mock.patch.object(pkg, "ignored_packages", lambda: {"linux"}), \
                mock.patch.object(pkg, "local_packages", lambda db: dict(installed)):
            updates, newest = pkg.repo_updates()
        self.assertEqual(newest, {"openssl": "4.1.0-1", "tor": "0.4.9.16-1", "linux": "7.4-1", "zlib": "1.3-1"})
        # Held back: linux. Not in the view: bash. Equal: zlib.
        self.assertEqual(updates, [("openssl", "3.6.5-1.1", "4.1.0-1"), ("tor", "0.4.9.14-1.1", "0.4.9.16-1")])

    @unittest.skipUnless(HAVE_PACMAN, "pacman is not installed here")
    def test_a_repository_lookup_reads_the_fetched_databases_a_bare_name_the_stack(self):
        # The plan pins a tracked package's official stable source by asking
        # [extra] itself (extra/labwc). The stack's view has no [extra] labwc
        # once CachyOS has rebuilt it (0.20.2-1.1 beats 0.20.2-1).
        conf = self.root / "pacman.conf"
        conf.write_text("[options]\nArchitecture = x86_64\nSigLevel = Never\n"
                        "[cachyos-extra-v3]\nServer = file:///nonexistent\n"
                        "[extra]\nServer = file:///nonexistent\n")
        (self.checked / "sync").mkdir(parents=True)
        local_db(self.checked / "local")
        local_db(self.db / "local")
        sync_db(self.checked / "sync/cachyos-extra-v3.db", [("labwc", "0.20.2-1.1")])
        sync_db(self.checked / "sync/extra.db", [("labwc", "0.20.2-1"), ("foot", "1.24-1")])
        stack = load("stack_for_views", LINK / "locachy_stack.py")
        with mock.patch.object(pkg, "PACMAN_CONFIG", str(conf)), \
                mock.patch.object(pkg, "helper_module", lambda name: stack):
            pkg.merge_checked()
            self.assertEqual(pkg.repo_info("extra/labwc")[:2], ("extra", "0.20.2-1"))
            self.assertEqual(pkg.repo_info("labwc")[:2], ("cachyos-extra-v3", "0.20.2-1.1"))
            self.assertEqual(pkg.repo_info("foot")[:2], ("extra", "1.24-1"))
            self.assertIsNone(pkg.repo_info("extra/absent"))

    @unittest.skipUnless(HAVE_PACMAN, "pacman is not installed here")
    def test_the_record_decides_whether_the_host_databases_are_the_published_ones(self):
        stack = load("stack_for_record", LINK / "locachy_stack.py")
        stack.RECORD = str(self.root / "sync-published.json")
        conf = self.root / "pacman.conf"
        conf.write_text("[options]\nArchitecture = x86_64\n[core]\nServer = file:///nonexistent\n")
        (self.db / "sync").mkdir()
        (self.db / "sync/core.db").write_bytes(b"core as LOCachy published it")
        with mock.patch.object(pkg, "helper_module", lambda name: stack), \
                mock.patch.object(pkg, "PACMAN_CONFIG", str(conf)):
            self.assertIsNone(pkg.host_databases_mismatch())  # nothing published here yet
            stack.record(str(self.db / "sync"), stack.RECORD)
            self.assertIsNone(pkg.host_databases_mismatch())
            (self.db / "sync/core.db").write_bytes(b"core as a plain pacman -Sy fetched it")
            self.assertIn("core.db is not the database LOCachy published", pkg.host_databases_mismatch())

    def test_locachy_installs_run_only_on_the_published_databases(self):
        calls = []
        err = io.StringIO()
        real_exists = os.path.exists
        with mock.patch.object(pkg, "host_databases_mismatch",
                               lambda: "extra.db is not the database LOCachy published"), \
                mock.patch.object(pkg.subprocess, "run",
                                  lambda *a, **k: calls.append(a[0]) or subprocess.CompletedProcess(a[0], 0)), \
                mock.patch.object(pkg.os.path, "exists",
                                  lambda path: False if path == "/run/netns/locachy-net" else real_exists(path)), \
                contextlib.redirect_stderr(err):
            self.assertFalse(pkg.run_pacman("pacman -S --needed --noconfirm foot"))
            self.assertTrue(pkg.run_pacman("pacman -D --asdeps foot >/dev/null 2>&1"))
        self.assertEqual(calls, ["pacman -D --asdeps foot >/dev/null 2>&1"])  # only the -D ran
        self.assertIn("check for updates and apply", err.getvalue())

    def test_queries_read_the_stacks_view_once_checked(self):
        with mock.patch.object(pkg, "PACMAN_CONFIG", "/fixture/pacman.conf"):
            self.assertEqual(pkg.sync_options(), ["--config", "/fixture/pacman.conf"])
            (self.merged / "sync").mkdir(parents=True)
            self.assertEqual(pkg.sync_options(), ["--config", "/fixture/pacman.conf", "--dbpath", str(self.merged)])
            self.assertEqual(pkg.sync_options(str(self.checked)),
                             ["--config", "/fixture/pacman.conf", "--dbpath", str(self.checked)])

    @unittest.skipUnless(shutil.which("pacman-conf"), "pacman-conf is not installed here")
    def test_held_packages_come_from_pacman_conf(self):
        conf = self.root / "pacman.conf"
        conf.write_text("[options]\nIgnorePkg = linux linux-headers\n")
        with mock.patch.object(pkg, "PACMAN_CONFIG", str(conf)):
            self.assertEqual(pkg.ignored_packages(), {"linux", "linux-headers"})

    def test_nothing_is_published_without_checked_databases_or_while_pacman_runs(self):
        with mock.patch.object(pkg, "helper_module", lambda name: tx):
            with self.assertRaisesRegex(RuntimeError, "check for updates first"):
                pkg.publish_merged()
            (self.merged / "sync").mkdir(parents=True)
            (self.merged / "sync/core.db").write_bytes(b"checked core")
            (self.db / "db.lck").write_text("a running pacman")
            with self.assertRaisesRegex(tx.TransactionError, "database is locked"):
                pkg.publish_merged()
        self.assertFalse((self.db / "sync").exists())
        self.assertEqual((self.db / "db.lck").read_text(), "a running pacman")


if __name__ == "__main__":
    unittest.main()
