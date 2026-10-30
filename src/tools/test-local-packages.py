#!/usr/bin/env python3
"""Local recipe tracking without pacman/network/root: subprocess calls are
mocked, and recipes/records live in a supplied scratch directory. No recipe
is executed while showing status or making an update plan.
  python3 -B tools/test-local-packages.py SCRATCH_PARENT
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
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1] / "link/locachy-pkg"
loader = importlib.machinery.SourceFileLoader("local_package_tool", str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
pkg = importlib.util.module_from_spec(spec)
loader.exec_module(pkg)
ON_HOST_ROOT = pkg.on_host_root
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class LocalPackages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="local-packages-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "src"
        self.recipe = self.src / "pkgbuilds/labwc-locachy"
        self.recipe.mkdir(parents=True)
        self.marker = self.root / "must-not-run"
        (self.recipe / "PKGBUILD").write_text(
            "pkgname=labwc-locachy\npkgver=0.20.2\npkgrel=1.1\n"
            "pkgdesc='Local compositor'\n"
            f"touch '{self.marker}'\n", encoding="utf-8")
        (self.recipe / "guard.patch").write_text("patch version one\n")
        self.records = self.root / "records"
        self.records.mkdir()
        self.conf = self.root / "containers"
        self.conf.mkdir()
        self.manifest = self.root / "packages"
        self.manifest.write_text("# keep this\nlabwc-locachy\n")
        self.host = {"base": "1", "labwc-locachy": "0.20.2-1.1"}
        self.built_host = self.root / "built-host" # LOCachy's programs: built against this host
        self.built_host.write_text("host-current\n")
        self.level = self.root / "level"
        self.level.write_text("x86-64-v2\n")
        self.cpu = self.root / "cpu"
        self.rc = self.root / "rc.conf"
        self.source_repo = self.root / "repo"
        self.source_commit = self.root / "source-commit"
        self.signers = self.root / "trusted-signers"
        for name, value in (("SRC", str(self.src)), ("LOCAL_RECORDS", str(self.records)),
                            ("CONF_DIR", str(self.conf)), ("MANIFEST", str(self.manifest)),
                            ("BUILT_HOST_FILE", str(self.built_host)), ("LEVEL_FILE", str(self.level)),
                            ("CPU_FILE", str(self.cpu)),
                            ("RC_CONF", str(self.rc)), ("SOURCE_REPO_DIR", str(self.source_repo)),
                            ("SOURCE_COMMIT_FILE", str(self.source_commit)),
                            ("TRUSTED_SIGNERS", str(self.signers)),
                            ("SOURCE_UNBUILT_FILE", str(self.root / "source-unbuilt"))):
            p = mock.patch.object(pkg, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.patch("local_packages", lambda db: dict(self.host) if db == pkg.LOCAL_DB else {})
        self.patch("repo_updates", lambda: ([], {"base": "1"}))
        self.patch("aur_versions", lambda names: self.fail(f"AUR must not be queried for {names}"))
        self.patch("host_hash", lambda: "host-current")
        self.patch("build_host_hash", lambda path, recipes=None: "build-current")
        self.patch("on_host_root", lambda: False)

    def patch(self, name, value):
        p = mock.patch.object(pkg, name, value)
        p.start()
        self.addCleanup(p.stop)
        return p

    def record(self, **changes):
        data = {"name": "labwc-locachy", "recipe": pkg.recipe_hash(str(self.recipe)),
                "host": "build-current", "version": self.host["labwc-locachy"]}
        data.update(changes)
        (self.records / "labwc-locachy.json").write_text(json.dumps(data))

    def test_info_is_local_without_executing_recipe(self):
        self.patch("repo_info", lambda name: None)
        self.patch("aur_info", lambda names: self.fail(f"unexpected AUR lookup {names}"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pkg.cmd_info(["labwc-locachy"]), 0)
        self.assertIn("labwc-locachy\tlocal\tLOCachy\t0.20.2-1.1\tLocal compositor", out.getvalue())
        self.assertFalse(self.marker.exists())

    def test_srcinfo_and_literal_fallback_do_not_source_shell(self):
        self.assertEqual(pkg.recipe_info("labwc-locachy", str(self.recipe)),
                         ("0.20.2-1.1", "Local compositor"))
        (self.recipe / ".SRCINFO").write_text("pkgbase = labwc-locachy\n\tpkgver = 0.20.3\n\tpkgrel = 2\n\tepoch = 1\n\tpkgdesc = Checked metadata\n")
        self.assertEqual(pkg.recipe_info("labwc-locachy", str(self.recipe)),
                         ("1:0.20.3-2", "Checked metadata"))
        self.assertFalse(self.marker.exists())

    def test_recipe_hash_tracks_patch_content_and_rejects_links(self):
        old = pkg.recipe_hash(str(self.recipe))
        (self.recipe / "guard.patch").write_text("patch version two\n")
        self.assertNotEqual(old, pkg.recipe_hash(str(self.recipe)))
        (self.recipe / "outside").symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            pkg.recipe_hash(str(self.recipe))

    def test_clean_local_build_not_requeried_from_aur(self):
        self.record()
        self.assertEqual(pkg.plan(), [])

    def test_check_can_replace_a_stale_active_recipe_after_overlay_change(self):
        self.record()
        (self.recipe / "upstream.json").write_text('{"schema":1}\n')
        candidate = {"baseline_dir": str(self.recipe), "recipe_dir": str(self.recipe),
                     "candidate": {"candidate_id": "new", "package_version": "1:0.21.0-1",
                                   "repository_version": "0.21.0-1.7", "source_version": "0.21.0-1",
                                   "upstream_tag": "0.21.0-1"}}
        self.patch("checked_local_candidates", lambda recipes, installed: ({"labwc-locachy": candidate}, {}))
        self.patch("local_recipes", lambda: self.fail("Check read the stale promotion before pinning its replacement"))
        transaction = types.SimpleNamespace(snapshot=lambda *args, **kwargs: {"directory": "plan"})
        self.patch("helper_module", lambda name: transaction)
        self.patch("transaction_host_hash", lambda: "current")
        rows = pkg.plan()
        self.assertIn(("host", "local", "labwc-locachy", "0.20.2-1.1",
                       "1:0.21.0-1: stable 0.21.0-1 (0.21.0-1)"), rows)

    def test_check_plans_the_host_with_the_installed_source_and_pins_declared_packages(self):
        self.record()
        (self.recipe / "upstream.json").write_text('{"schema":1}\n')
        commit = "a" * 40
        candidate = {"baseline_dir": str(self.recipe), "recipe_dir": str(self.recipe),
                     "candidate": {"candidate_id": "new", "package_version": "1:0.20.2-1",
                                   "source_version": "0.20.2-1", "upstream_tag": "0.20.2-1"}}
        self.patch("check_source_update", lambda: ("old", commit, "new sources", "main"))
        self.patch("checked_local_candidates", lambda *args: ({"labwc-locachy": candidate}, {}))
        self.patch("repo_updates", lambda: ([], {"base": "1", "libraw": "0.22-1"}))
        self.manifest.write_text("labwc-locachy\nlibraw\nfrom-the-aur\n")
        snapshots = []
        transaction = types.SimpleNamespace(snapshot=lambda *args, **kwargs:
                                            snapshots.append(kwargs) or {"directory": "plan"})
        self.patch("helper_module", lambda _: transaction)
        self.patch("transaction_host_hash", lambda: "current")
        rows = pkg.plan()
        # Never the pending source's tests with the installed source's recipes.
        self.assertEqual(snapshots[0]["test_source"], str(self.src))
        # Repository packages the manifest declares: in the prepared transaction.
        self.assertEqual(snapshots[0]["missing"], ["libraw"])
        self.assertIn(("host", "missing", "from-the-aur", "", "declared, not installed"), rows)
        self.assertIn(("locachy", "note", "applied before the host: the rest is checked again by the updated updater",
                       "", ""), rows)
        self.assertFalse(hasattr(pkg, "staged_test_source"))

    def source_update_fixture(self, gate):
        """A source tree whose libinput recipe and contract name gate; its
        view module has libraw, which the published one did not."""
        tree = self.root / ("source-" + gate)
        recipe = tree / "pkgbuilds/libinput"
        recipe.mkdir(parents=True)
        (recipe / "upstream.json").write_text(json.dumps({"schema": 1, "gate": gate}))
        (tree / "tools").mkdir()
        (tree / "tools/contract-id").write_text(gate + "\n")
        (tree / "link").mkdir()
        (tree / "link/declare-packages").write_text((SOURCE.parent / "declare-packages").read_text())
        (tree / "link/modules.json").write_text(json.dumps({"view": {"packages": ["libmupdf", "libraw"]}}))
        (tree / "link/declared-packages-baseline.json").write_text(
            json.dumps({"base": ["base"], "modules": {"view": ["libmupdf"]}}))
        (tree / "packages").write_text("base\n")
        return tree

    def test_combined_update_installs_the_source_first_then_plans_the_host_with_it(self):
        """The FV1 failure: recipes from the installed source (gate v1), the
        contract from the update's (v2), and a library the update's view
        needs declared nowhere. Now one source plans, builds and tests."""
        shutil.rmtree(self.src)
        shutil.copytree(self.source_update_fixture("libinput-circular-scroll-v1"), self.src)
        new = self.source_update_fixture("libinput-circular-scroll-v2")
        self.manifest.write_text("base\nlibmupdf\nlibinput\n")
        self.host = {"base": "1", "libmupdf": "1", "libinput": "1:1.32.0-1"}
        commit = "b" * 40
        update = [("old", commit, "new sources", "main")]
        self.patch("check_source_update", lambda: update[0] if update else None)
        self.patch("repo_updates", lambda: ([], {"base": "1", "libmupdf": "1", "libraw": "0.22-1"}))

        def candidates(recipes, installed):
            gate = json.loads(Path(recipes["libinput"], "upstream.json").read_text())["gate"]
            return {"libinput": {"baseline_dir": recipes["libinput"], "recipe_dir": recipes["libinput"],
                                 "candidate": {"candidate_id": "c", "package_version": "1:1.32.0-1",
                                               "source_version": "1.32.0-1", "upstream_tag": "1.32.0-1",
                                               "gate": gate}}}, {}
        self.patch("checked_local_candidates", candidates)
        contracts = []

        def snapshot(*args, test_source=None, missing=None, **kwargs):
            # What locachy-build --validate-prepared compares: the candidate's
            # gate (its recipe) and the id its contract writes (the tests).
            contracts.append((args[4]["libinput"]["candidate"]["gate"],
                              Path(test_source, "tools/contract-id").read_text().strip(), missing))
            return {"directory": "plan"}
        transaction = types.SimpleNamespace(snapshot=snapshot, file_hash=lambda path: "hash",
                                            write_json=lambda path, data: None)
        self.patch("helper_module", lambda name: transaction)
        self.patch("transaction_host_hash", lambda: "current")
        self.patch("local_record", lambda name: {})
        unbuilt = self.root / "source-unbuilt"
        self.patch("PLAN_FILE", str(self.root / "plan"))
        pkg.save_plan(pkg.plan())  # Check
        self.assertEqual(contracts[0][:2], ("libinput-circular-scroll-v1",) * 2)
        record = self.root / "declared-packages.json"
        commands = []

        def run(cmd, net=False):
            commands.append(cmd)
            if cmd[0] == "python3":  # declare-packages itself, its record in the scratch tree
                return subprocess.run([sys.executable] + cmd[1:] + [str(record)], capture_output=True).returncode == 0
            return True
        self.patch("run", run)

        def install_source(c):
            shutil.rmtree(self.src)
            shutil.copytree(new, self.src)
        self.patch("apply_source_update", install_source)

        class Exec(Exception):
            pass

        def execv(path, argv):
            raise Exec(path, argv)
        p = mock.patch.object(pkg.os, "execv", execv)
        p.start()
        self.addCleanup(p.stop)
        with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(Exec) as phase_two:
            pkg.cmd_apply(["host", "locachy"])
        self.assertEqual(phase_two.exception.args, (pkg.SELF, [pkg.SELF, "apply", "--after-source", "host", "locachy"]))
        self.assertEqual(commands, [["sh", os.path.join(str(self.src), "link/install-pkg-tools"), str(self.src)],
                                    ["python3", "-I", os.path.join(str(self.src), "link/declare-packages"),
                                     str(self.src), str(self.manifest)]])
        self.assertEqual(unbuilt.read_text(), commit + "\n")
        self.assertIn("libraw", pkg.load_manifest(str(self.manifest)))
        self.assertIn("===> locachy: source installed; the rest is checked again by the updated updater",
                      out.getvalue())
        # The updated updater: one source for recipes and contracts, the
        # declared library in the prepared transaction, the programs built.
        del update[:]
        applied = []
        self.patch("apply_target", lambda target, rows: applied.append(target) or True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply(["host", "locachy"], after_source=True), 0)
        self.assertEqual(contracts[1], ("libinput-circular-scroll-v2", "libinput-circular-scroll-v2", ["libraw"]))
        self.assertEqual(applied, ["host", "locachy"])
        self.assertEqual(len(contracts), 3)  # Check, phase two, the plan after it
        rows, _ = pkg.load_plan()
        self.assertIn(("locachy", "rebuild",
                       "its programs: not yet built from the installed LOCachy source (make.sh --boot)", "", ""), rows)

    def source_setup_failure_case(self, helper):
        """A helper fails after the source is recorded; a fresh Check and
        Apply, not the preserved old plan, must finish setup before the host."""
        self.record()
        self.patch("PLAN_FILE", str(self.root / "plan"))
        self.patch("repo_updates", lambda: ([], {"base": "1", "libraw": "0.22-1"}))
        commit = "b" * 40
        commands = []
        failed = [False]

        def run(command, net=False):
            commands.append(command)
            if command[0] == helper and not failed[0]:
                failed[0] = True
                return False
            if command[0] == "python3":
                self.manifest.write_text("labwc-locachy\nlibraw\n")
            return True
        self.patch("run", run)
        self.patch("apply_source_update", lambda c: self.source_commit.write_text(c + "\n"))
        rows = [("host", "repo", "base", "1", "2"), ("locachy", "rebuild", "new source", "old", commit)]
        pkg.save_plan(rows)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply([]), 1)
        self.assertEqual(self.source_commit.read_text(), commit + "\n")
        self.assertTrue(pkg.source_setup_pending())
        self.assertEqual(len(commands), 1 if helper == "sh" else 2)
        # The original failure plan is gone. A new Check must work even
        # while this process still has the old updater's helper modules.
        Path(pkg.PLAN_FILE).unlink()
        self.patch("check_source_update", lambda: self.fail("Check fetched a source during pending setup"))
        self.patch("checked_local_candidates", lambda *args: self.fail("Check used helpers before setup finished"))
        self.patch("repo_updates", lambda: self.fail("Check did repository work before setup finished"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_plan([]), 0)
        pending, _ = pkg.load_plan()
        self.assertEqual(pkg.targets_of(pending), ["host", "locachy"])
        self.assertTrue(any("source setup did not finish" in r[2] for r in pending))
        commands.clear()

        class Exec(Exception):
            pass

        def execv(path, argv):
            raise Exec(path, argv)
        with mock.patch.object(pkg.os, "execv", execv), contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaises(Exec) as phase_two:
            pkg.cmd_apply([])
        self.assertEqual(phase_two.exception.args, (pkg.SELF, [pkg.SELF, "apply", "--after-source"]))
        self.assertEqual([c[0] for c in commands], ["sh", "python3"])
        self.assertFalse(pkg.source_setup_pending())
        self.assertIn("libraw", pkg.load_manifest(str(self.manifest)))
        self.assertEqual(Path(pkg.SOURCE_UNBUILT_FILE).read_text(), commit + "\n")
        # Only after the retry succeeded may the refreshed host plan run.
        self.patch("repo_updates", lambda: ([], {"base": "1", "libraw": "0.22-1"}))
        self.patch("checked_local_candidates", lambda *args: ({}, {}))
        self.patch("check_source_update", lambda: None)
        applied = []
        self.patch("apply_target", lambda target, rows: applied.append(target) or True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply([], after_source=True), 0)
        self.assertEqual(applied, ["host", "locachy"])

    def test_failed_helper_install_resumes_after_a_fresh_check(self):
        self.source_setup_failure_case("sh")

    def test_failed_declarations_resume_after_a_fresh_check(self):
        self.source_setup_failure_case("python3")

    def test_phase_two_does_not_apply_a_newer_source_discovered_at_refresh(self):
        self.record()
        self.patch("PLAN_FILE", str(self.root / "plan"))
        installed, newer = "b" * 40, "c" * 40
        self.source_commit.write_text(installed + "\n")
        Path(pkg.SOURCE_UNBUILT_FILE).write_text(installed + "\n")
        fetched = []
        applied = []
        self.patch("check_source_update", lambda: fetched.append(newer) or (installed[:7], newer, "newer", "main"))
        self.patch("apply_source_update", lambda c: self.fail("phase two applied an unselected newer source"))

        def apply(target, rows):
            # The initial phase-two refresh must not fetch another source.
            self.assertEqual(fetched, [])
            applied.append((target, rows))
            return True
        self.patch("apply_target", apply)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply(["locachy"], after_source=True), 0)
        self.assertEqual([t for t, _ in applied], ["locachy"])
        self.assertFalse(any(r[4] == newer for _, rows in applied for r in rows))
        self.assertEqual(fetched, [newer])  # the final Check offers it for a later Apply
        remaining, _ = pkg.load_plan()
        self.assertTrue(any(r[:2] == ("locachy", "rebuild") and r[4] == newer for r in remaining))
        self.assertEqual(self.source_commit.read_text(), installed + "\n")

    def test_unbuilt_source_is_built_even_against_the_same_host(self):
        self.record()
        unbuilt = self.root / "source-unbuilt"
        self.assertEqual(pkg.plan(), [])
        unbuilt.write_text("b" * 40 + "\n")
        self.assertEqual([r[:2] for r in pkg.plan()], [("locachy", "rebuild")])
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or True)
        self.patch("on_host_root", lambda: True)
        self.assertTrue(pkg.rebuild_locachy())
        self.assertEqual(calls, [["sh", os.path.join(str(self.src), "make.sh"), "--boot", "x86-64-v2", "x86-64-v2"]])
        make = (SOURCE.parents[1] / "make.sh").read_text()
        self.assertLess(make.index('>"$lib/built-host"'), make.index('rm -f "$lib/source-unbuilt"'))
        self.assertIn('python3 -I "$src/link/declare-packages" "$src" /etc/locachy/packages', make)

    def test_host_only_apply_keeps_the_installed_source_and_failed_source_stops(self):
        rows = [("host", "repo", "base", "1", "2"), ("locachy", "repo", "locachy-src", "old", "new"),
                ("locachy", "rebuild", "new source", "old", "c" * 40)]
        applied = []
        self.patch("load_plan", lambda: (rows, "checked"))
        self.patch("apply_target", lambda target, r: applied.append(target) or True)
        self.patch("plan", lambda: [])
        self.patch("save_plan", lambda r: None)
        self.patch("install_source_first", lambda c: self.fail("the source was installed for a host-only apply"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_apply(["host"]), 0)
        self.assertEqual(applied, ["host"])
        self.patch("install_source_first", lambda c: False)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(pkg.cmd_apply([]), 1)
        self.assertEqual(applied, ["host"])  # nothing after a failed source
        self.assertIn("===> not done: locachy", out.getvalue())

    def test_check_uses_official_stable_recipe_not_optimized_build_release(self):
        (self.recipe / "upstream.json").write_text('{"package":"labwc"}\n')
        queries, prepared = [], []
        def info(name):
            queries.append(name)
            return ("extra", "0.20.2-1", "stable") if name == "extra/labwc" else \
                   ("cachyos-extra-v3", "0.20.2-1.1", "optimized")
        self.patch("repo_info", info)
        def prepare(*args, **kwargs):
            prepared.append((args, kwargs))
            return self.root / "locked/lock.json"
        upstream = types.SimpleNamespace(prepare_candidate=prepare, validate_candidate=lambda *args: {"candidate_id": "locked"})
        self.patch("helper_module", lambda name: upstream)
        selected, pins = pkg.checked_local_candidates({"labwc-locachy": str(self.recipe)}, self.host)
        self.assertEqual(queries, ["extra/labwc", "labwc"])
        self.assertEqual(prepared[0][0][2:4], ("cachyos-extra-v3", "0.20.2-1.1"))
        self.assertEqual(prepared[0][1]["stable_version"], "0.20.2-1")
        self.assertEqual(selected["labwc-locachy"]["candidate"]["candidate_id"], "locked")
        self.assertEqual(pins["labwc-locachy"], str(self.root / "locked/lock.json"))
        self.patch("repo_info", lambda name: ("extra-testing", "0.21-1", "testing"))
        with self.assertRaisesRegex(RuntimeError, "official extra stable source"):
            pkg.checked_local_candidates({"labwc-locachy": str(self.recipe)}, self.host)
        self.assertEqual(len(prepared), 1)

    def test_apply_uses_pinned_replacement_not_the_stale_promotion(self):
        (self.recipe / "upstream.json").write_text('{"schema":1}\n')
        self.patch("local_recipes", lambda: self.fail("Apply used a stale active recipe"))
        calls = []
        self.patch("apply_prepared_host", lambda rows, recipes: calls.append(recipes) or True)
        self.patch("repo_info", lambda name: self.fail("Apply re-resolved stable or target versions"))
        self.assertTrue(pkg.apply_target("host", []))
        self.assertEqual(calls, [{"labwc-locachy": str(self.recipe)}])

    def test_plan_recipe_change_same_version(self):
        self.record()
        (self.recipe / "guard.patch").write_text("new patch, same pkgver\n")
        rows = pkg.plan()
        self.assertIn(("host", "local", "labwc-locachy", "0.20.2-1.1", "local recipe or patch changed"), rows)
        self.assertTrue(any(r[0] == "locachy" for r in rows))

    def test_plan_rebuilds_locachy_built_against_another_host(self):
        self.record()
        self.built_host.write_text("host-older\n")
        self.assertEqual([r[:2] for r in pkg.plan()], [("locachy", "rebuild")])
        self.built_host.unlink()
        self.assertEqual([r[:2] for r in pkg.plan()], [("locachy", "rebuild")])

    def test_rebuild_locachy_only_on_the_host_and_when_the_host_changed(self):
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or True)
        self.patch("on_host_root", lambda: True)
        self.assertTrue(pkg.rebuild_locachy())
        self.assertEqual(calls, []) # built against the host as it is
        make = ["sh", os.path.join(str(self.src), "make.sh"), "--boot", "x86-64-v2", "x86-64-v2"]
        self.assertTrue(pkg.rebuild_locachy(force=True))
        self.assertEqual(calls, [make]) # no CPU recorded: the level's
        calls.clear()
        self.cpu.write_text("ivybridge\n")
        self.assertTrue(pkg.rebuild_locachy(force=True))
        self.assertEqual(calls, [make[:-1] + ["ivybridge"]])
        calls.clear()
        self.cpu.unlink()
        self.assertTrue(pkg.rebuild_locachy(force=True))
        self.assertEqual(calls, [make])
        self.built_host.write_text("host-older\n")
        self.assertTrue(pkg.apply_target("locachy", [("locachy", "rebuild", "its programs", "", "")]))
        self.assertEqual(calls, [make, make])
        self.patch("on_host_root", lambda: False) # a container's or a build's root
        self.assertTrue(pkg.rebuild_locachy(force=True))
        self.assertEqual(len(calls), 2)

    def test_host_root_is_process_one_s_root_with_locachy_init(self):
        root = os.stat("/")
        other = os.stat(str(self.root))
        for exe, stats, expected in ((pkg.INIT, (root, root), True), ("/init", (root, root), True),
                                     ("/usr/bin/bash", (root, root), False),
                                     ("/usr/lib/systemd/systemd", (root, root), False),
                                     (pkg.INIT, (other, root), False), ("/init", (other, root), False)):
            with mock.patch.object(pkg.os, "readlink", lambda path, exe=exe: exe), \
                 mock.patch.object(pkg.os, "stat", lambda path, stats=stats: stats[0] if path == "/" else stats[1]):
                self.assertEqual(ON_HOST_ROOT(), expected, exe)

    def test_plan_toolchain_change_and_version_mismatch(self):
        self.record(host="older-build-host")
        self.assertIn("host libraries or build tools changed", pkg.plan()[0][4])
        self.record(version="0.20.1-1")
        self.assertIn("installed version differs", pkg.plan()[0][4])

    def test_repo_update_schedules_local_rebuild_after_pacman(self):
        self.record()
        self.patch("repo_updates", lambda: ([("base", "1", "2")], {"base": "2"}))
        rows = pkg.plan()
        self.assertTrue(any(r[:3] == ("host", "local", "labwc-locachy") for r in rows))
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append((cmd, net)) or True)
        self.assertTrue(pkg.apply_target("host", rows))
        self.assertEqual(calls[0], (["pacman", "-Syu", "--noconfirm"], True))
        self.assertEqual(calls[1][0], [pkg.BUILD_TOOL, "-i", "-r", "--local", "labwc-locachy"])

    def test_apply_rechecks_library_changes_not_in_plan(self):
        self.record(host="old")
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or True)
        self.assertTrue(pkg.apply_target("host", []))
        self.assertEqual(calls[-1], [pkg.BUILD_TOOL, "-i", "-r", "--local", "labwc-locachy"])

    def test_missing_recipe_never_falls_back_to_aur(self):
        self.record()
        (self.recipe / "PKGBUILD").unlink()
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or True)
        rows = [("host", "local", "labwc-locachy", "0.20.2-1.1", "patch changed")]
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(pkg.apply_target("host", rows))
        self.assertEqual(calls, [["pacman", "-Syu", "--noconfirm"]])

    def test_disappeared_recipe_stays_local_in_plan_and_info(self):
        self.record()
        (self.recipe / "PKGBUILD").unlink()
        rows = pkg.plan()
        self.assertIn(("host", "local", "labwc-locachy", "0.20.2-1.1", "local recipe is missing"), rows)
        self.patch("repo_info", lambda name: self.fail("local package queried in repository"))
        self.patch("aur_info", lambda names: self.fail("local package queried in AUR"))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            pkg.cmd_info(["labwc-locachy"])
        self.assertIn("\tlocal\tLOCachy\t0.20.2-1.1\tlocal recipe is missing", output.getvalue())

    def test_fingerprint_is_the_recipes_and_excludes_local_outputs(self):
        self.patch("build_host_hash", LocalPackages.original_build_hash)
        seen = []
        upstream = types.SimpleNamespace(build_fingerprint=lambda path, local, db: seen.append((path, local, db)) or "f")
        self.patch("helper_module", lambda name: upstream)
        self.assertEqual(pkg.build_host_hash(str(self.recipe), pkg.local_recipes()), "f")
        self.assertEqual(seen, [(str(self.recipe), {"labwc-locachy"}, pkg.LOCAL_DB)])

    def test_status_reports_local_rebuild_without_mutation(self):
        self.record(host="old")
        self.patch("load_manifest", lambda: ["labwc-locachy"])
        self.patch("load_installed_db", lambda: ({"labwc-locachy": []}, {"labwc-locachy": ["labwc-locachy"]}, {"labwc-locachy"}))
        with mock.patch.object(pkg.subprocess, "check_output", return_value=""):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(pkg.cmd_status(machine=True), 0)
        self.assertIn("local-rebuild\tlabwc-locachy", output.getvalue())
        self.assertEqual(self.manifest.read_text(), "# keep this\nlabwc-locachy\n")
        self.assertFalse(self.marker.exists())

    def test_github_source_and_recipe_update_and_trusted_signers(self):
        self.record()
        self.patch("in_net", lambda cmd: cmd)
        (self.src / "make.sh").write_text("#!/bin/sh\nexit 0\n")
        (self.src / "make.sh").chmod(0o755)
        origin = self.root / "origin"
        (origin / "src").mkdir(parents=True)
        for p in self.src.rglob("*"):
            rel = p.relative_to(self.src)
            dst = origin / "src" / rel
            if p.is_dir():
                dst.mkdir(parents=True, exist_ok=True)
            else:
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(p.read_bytes())
                dst.chmod(p.stat().st_mode & 0o777)
        git = lambda *a: pkg.subprocess.run(["git", "-C", str(origin), *a], check=True, capture_output=True, text=True).stdout.strip()
        git("init", "-b", "main")
        git("config", "user.name", "Test")
        git("config", "user.email", "test@example.com")
        git("add", "-A")
        git("commit", "-m", "Initial release")
        c1 = git("rev-parse", "HEAD")
        self.rc.write_text(f'update_repo="file://{origin}"\n')

        # Identical tree: no update planned, and source-commit is recorded.
        self.assertEqual(pkg.plan(), [])
        self.assertEqual(self.source_commit.read_text().strip(), c1)

        # Another branch is ignored until rc.conf follows it; a bad name is refused.
        git("checkout", "-q", "-b", "next")
        (origin / "src/containers").mkdir(exist_ok=True)
        (origin / "src/containers/next-only.conf").write_text('package="next-only"\n')
        git("add", "-A")
        git("commit", "-m", "Next branch only")
        c_next = git("rev-parse", "HEAD")
        git("checkout", "-q", "main")
        self.assertEqual(pkg.plan(), [])
        self.rc.write_text(f'update_repo="file://{origin}"\nupdate_branch="next"\n')
        rows = pkg.plan()
        self.assertIn(("locachy", "rebuild", "new source and recipes from branch next (make.sh --boot)",
                       c1[:7], c_next), rows)
        for bad in ("--upload-pack=x", "a..b", "main.lock", "x//y"):
            self.rc.write_text(f'update_repo="file://{origin}"\nupdate_branch="{bad}"\n')
            with self.assertRaisesRegex(RuntimeError, "not a branch name"):
                pkg.plan()
        self.rc.write_text(f'update_repo="file://{origin}"\n')

        # Upstream adds a new container recipe and updates make.sh.
        (origin / "src/containers").mkdir(exist_ok=True)
        (origin / "src/containers/signal.conf").write_text('package="signal-desktop"\n')
        (origin / "src/make.sh").write_text("#!/bin/sh\n# v2\nexit 0\n")
        (origin / "src/make.sh").chmod(0o755)
        git("add", "-A")
        git("commit", "-m", "Add Signal container recipe")
        c2 = git("rev-parse", "HEAD")

        rows = pkg.plan()
        self.assertIn(("locachy", "repo", "locachy-src", c1[:7], f"{c2[:7]} (Add Signal container recipe)"), rows)
        self.assertIn(("locachy", "rebuild", "new source and recipes from branch main (make.sh --boot)", c1[:7], c2), rows)

        rebuilt = []
        self.patch("rebuild_locachy", lambda force=False: rebuilt.append(force) or True)
        self.assertTrue(pkg.apply_target("locachy", rows))
        self.assertEqual(rebuilt, [True])
        self.assertTrue((self.src / "containers/signal.conf").is_file())
        self.assertEqual(self.source_commit.read_text().strip(), c2)
        self.record()
        self.assertEqual(pkg.plan(), [])

        # When trusted-signers is populated, an unsigned commit is rejected.
        self.signers.write_text("user@example.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n")
        (origin / "src/containers/untrusted.conf").write_text('package="bad"\n')
        git("add", "-A")
        git("commit", "-m", "Unsigned change")
        with self.assertRaisesRegex(RuntimeError, "signature verification"):
            pkg.plan()

    def test_upstream_requests_are_made_again_and_say_why_they_failed(self):
        self.patch("in_net", lambda cmd: cmd)
        self.patch("time", types.SimpleNamespace(sleep=lambda s: None))
        answers = []

        def curl(command, **kwargs):
            self.assertEqual(command[0], "curl")
            self.assertIn("--max-time", command)
            return answers.pop(0)
        self.patch("subprocess", types.SimpleNamespace(run=curl, TimeoutExpired=subprocess.TimeoutExpired))
        dropped = subprocess.CompletedProcess([], 35, b"", b"curl: (35) TLS connect error\n")
        answers[:] = [dropped, dropped, subprocess.CompletedProcess([], 0, b"{}", b"")]
        self.assertEqual(pkg.fetch_upstream("https://example.org/a", max_bytes=10, timeout=5), b"{}")
        answers[:] = [dropped, dropped, dropped]
        with self.assertRaisesRegex(RuntimeError, "https://example.org/a: curl: \\(35\\) TLS connect error"):
            pkg.fetch_upstream("https://example.org/a", max_bytes=10, timeout=5)
        answers[:] = [subprocess.CompletedProcess([], 63, b"", b"curl: (63) too big\n"), dropped]
        with self.assertRaisesRegex(RuntimeError, "more than 10 bytes"):
            pkg.fetch_upstream("https://example.org/a", max_bytes=10, timeout=5)
        self.assertEqual(len(answers), 1)  # too big: not asked again

    def test_unreachable_source_repo_is_a_note_in_the_plan(self):
        self.record()
        self.patch("in_net", lambda cmd: cmd)
        gone = self.root / "gone"
        self.rc.write_text(f'update_repo="file://{gone}"\n')
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rows = pkg.plan()
        self.assertEqual(len(rows), 1, rows)
        target, kind, what, old, new = rows[0]
        self.assertEqual((target, kind, old, new), ("locachy", "note", "", ""))
        self.assertTrue(what.startswith(f"source not checked: could not fetch file://{gone} (main): "), what)
        self.assertIn("locachy-pkg: LOCachy source not checked: could not fetch", err.getvalue())
        self.assertEqual(pkg.targets_of(rows), [])  # nothing to apply
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            pkg.print_plan(rows, "2026-10-27 10:00")
        self.assertIn("Note: locachy " + what + "\n", out.getvalue())
        self.assertIn("Everything is up to date.", out.getvalue())
        with mock.patch.dict(os.environ, {"LOCACHY_UPDATE_STRICT": "1"}), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaisesRegex(RuntimeError, "source repo fetch failed"):
            pkg.plan()

    def test_tracked_overlay_is_discovered_without_placeholder_recipe(self):
        (self.recipe / "PKGBUILD").unlink()
        (self.recipe / "upstream.json").write_text('{"schema":1}\n')
        self.assertEqual(pkg.baseline_recipes(), {"labwc-locachy": str(self.recipe)})
        self.assertEqual(pkg.recipe_info("labwc-locachy", str(self.recipe)),
                         ("", "LOCachy local package labwc-locachy"))
        self.assertFalse(self.marker.exists())

    def test_install_missing_local_leaf_uses_local_builder(self):
        self.host = {}
        self.patch("load_manifest", lambda: ["labwc-locachy"])
        self.patch("load_installed_db", lambda: ({}, {}, set()))
        calls = []
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or False)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pkg.cmd_sync(), 1)
        self.assertEqual(calls, [[pkg.BUILD_TOOL, "-i", "--local", "labwc-locachy"]])

    def apply_host(self, rows):
        """Apply with pacman doubled: (commands, package queries, result)."""
        calls, queries = [], []
        def pacman(command):
            calls.append(command)
            self.host["sof-firmware"] = "1"
            return True
        self.patch("run", lambda cmd, net=False: calls.append(cmd) or True)
        self.patch("run_pacman", pacman)
        query = lambda cmd, **kwargs: queries.append(cmd) or types.SimpleNamespace(returncode=0)
        with mock.patch.object(pkg.subprocess, "run", query), contextlib.redirect_stdout(io.StringIO()):
            result = pkg.apply_target("host", rows)
        return calls, queries, result

    def test_declared_missing_leaf_is_planned_shown_and_installed_by_apply(self):
        self.record()
        self.patch("repo_updates", lambda: ([], {"base": "1", "sof-firmware": "1"}))
        self.manifest.write_text("# keep this\nlabwc-locachy\nsof-firmware\n")
        rows = pkg.plan()
        missing = ("host", "missing", "sof-firmware", "", "declared, not installed")
        self.assertIn(missing, rows)
        self.assertEqual(pkg.targets_of(rows)[0], "host")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            pkg.print_plan(rows, "2026-10-28 10:00")
        self.assertIn("  install sof-firmware: declared, not installed\n", out.getvalue())
        calls, queries, result = self.apply_host(rows)
        self.assertTrue(result)
        # The host update first, then the leaf through sync's own repository path.
        self.assertEqual(calls, [["pacman", "-Syu", "--noconfirm"],
                                 "pacman -S --needed --noconfirm --assume-installed initramfs sof-firmware"])
        self.assertEqual(queries, [["pacman", "-Sp", "sof-firmware"]])
        self.assertNotIn(missing, pkg.plan())

    def test_apply_installs_nothing_when_no_declared_leaf_is_missing(self):
        self.record()
        self.assertFalse(any(r[1] == "missing" for r in pkg.plan()))
        calls, queries, result = self.apply_host([("host", "repo", "base", "1", "2")])
        self.assertTrue(result)
        self.assertEqual(calls, [["pacman", "-Syu", "--noconfirm"]])
        self.assertEqual(queries, [])
        # Planned, then no longer declared (removed) or already installed: left alone.
        rows = [("host", "missing", "sof-firmware", "", "declared, not installed")]
        calls, queries, result = self.apply_host(rows)
        self.assertTrue(result)
        self.assertEqual((calls, queries), ([["pacman", "-Syu", "--noconfirm"]], []))
        self.manifest.write_text("labwc-locachy\nsof-firmware\n")
        self.host["sof-firmware"] = "1"
        calls, queries, result = self.apply_host(rows)
        self.assertEqual((calls, queries, result), ([["pacman", "-Syu", "--noconfirm"]], [], True))

    def test_prepared_host_installs_missing_leaf_only_after_its_transaction(self):
        (self.recipe / "upstream.json").write_text('{"schema":1}\n')
        self.manifest.write_text("labwc-locachy\nsof-firmware\n")
        self.patch("local_recipes", lambda: {"labwc-locachy": str(self.recipe)})
        rows = [("host", "missing", "sof-firmware", "", "declared, not installed")]
        done = []
        self.patch("apply_prepared_host", lambda rows, recipes: done.append("prepared") or True)
        calls, queries, result = self.apply_host(rows)
        self.assertTrue(result)
        self.assertEqual(done + calls, ["prepared",
                                        "pacman -S --needed --noconfirm --assume-installed initramfs sof-firmware"])
        del self.host["sof-firmware"]
        self.patch("apply_prepared_host", lambda rows, recipes: False)
        calls, queries, result = self.apply_host(rows)
        self.assertEqual((calls, queries, result), ([], [], False))


LocalPackages.original_build_hash = staticmethod(pkg.build_host_hash)
if __name__ == "__main__":
    unittest.main()
