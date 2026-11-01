#!/usr/bin/env python3
"""Fresh-root reconciliation order and failure boundaries; no real pacman.
  python3 -B tools/test-pkg-sync.py SCRATCH_PARENT
"""
import contextlib
import importlib.machinery
import importlib.util
import io
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("sync_package_tool", str(SOURCE / "link/locachy-pkg"))
spec = importlib.util.spec_from_loader(loader.name, loader)
pkg = importlib.util.module_from_spec(spec)
loader.exec_module(pkg)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
INSTALL_TRACKED = pkg.install_tracked
LOAD_INSTALLED = pkg.load_installed_db


class Sync(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pkg-sync-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.calls = []
        self.installed = {"base": [], "bootstrap-only": []}
        self.leaves = ["base", "repo-program", "libinput-tools", "arbitrary-aur-name"]
        self.baseline = {"libinput-tools": str(self.root / "recipe")}
        self.patch("load_manifest", lambda: self.leaves)
        self.patch("baseline_recipes", lambda: self.baseline)
        self.patch("local_recipes", lambda: self.baseline)
        self.patch("tracked_recipes", lambda recipes: recipes)
        self.patch("local_names", lambda recipes=None: set(self.baseline))
        self.patch("local_packages", lambda db: {n: "1" for n in self.installed})
        self.patch("local_rebuilds", lambda *args: [])
        self.patch("load_installed_db", lambda: (dict(self.installed), {n: [n] for n in self.installed},
                                                set(self.installed)))
        self.patch("install_tracked", self.tracked)
        self.patch("run_pacman", self.pacman)
        self.patch("run", self.run_command)
        p = mock.patch.object(pkg.subprocess, "run", self.query)
        p.start()
        self.addCleanup(p.stop)
        p = mock.patch.object(pkg.subprocess, "check_output", return_value="")
        p.start()
        self.addCleanup(p.stop)

    def patch(self, name, value):
        p = mock.patch.object(pkg, name, value)
        p.start()
        self.addCleanup(p.stop)

    def query(self, cmd, **kwargs):
        self.calls.append(cmd)
        return types.SimpleNamespace(returncode=0 if cmd[-1] == "repo-program" else 1)

    def pacman(self, command):
        self.calls.append(command)
        args = shlex.split(command)
        if "-S" in args:
            self.installed["repo-program"] = []
        if "-Rns" in args:
            self.installed.pop("bootstrap-only", None)
        return True

    def tracked(self, names):
        self.calls.append(("tracked", set(names)))
        self.assertIn("repo-program", self.installed)
        self.installed["libinput-tools"] = []
        return True

    def run_command(self, cmd, net=False):
        self.calls.append((cmd, net))
        self.assertIn("libinput-tools", self.installed)
        self.installed["arbitrary-aur-name"] = []
        return True

    def sync(self, first_install=False):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(pkg.os.path, "isfile", return_value=True):
            return pkg.cmd_sync(first_install)

    def aur_changes_tracked_inputs(self):
        """libinput-tools' recorded inputs stop matching once the AUR package
        (and what it depends on) is installed, until it is built again."""
        rebuilt = []
        def tracked(names):
            rebuilt.append("arbitrary-aur-name" in self.installed)
            return self.tracked(names)
        self.patch("install_tracked", tracked)
        self.patch("local_rebuilds", lambda *args: [("libinput-tools", "host libraries or build tools changed")]
                   if "arbitrary-aur-name" in self.installed and not (rebuilt and rebuilt[-1]) else [])
        return rebuilt

    def test_repo_then_validated_tools_then_aur_then_prune(self):
        self.assertEqual(self.sync(), 0)
        repo = next(i for i, c in enumerate(self.calls) if isinstance(c, str) and "pacman -S " in c)
        tracked = self.calls.index(("tracked", {"libinput-tools"}))
        aur = self.calls.index(([pkg.BUILD_TOOL, "-i", "arbitrary-aur-name"], True))
        prune = next(i for i, c in enumerate(self.calls) if isinstance(c, str) and "pacman -Rns " in c)
        self.assertLess(repo, tracked)
        self.assertLess(tracked, aur)
        self.assertLess(aur, prune)
        self.assertIn("--assume-installed initramfs", self.calls[repo])
        self.calls.clear()
        self.assertEqual(self.sync(), 0)
        self.assertFalse(any(isinstance(c, tuple) for c in self.calls))
        self.assertFalse(any(isinstance(c, str) and "pacman -S " in c for c in self.calls))

    def test_repo_failure_stops_before_build_or_prune(self):
        self.patch("run_pacman", lambda cmd: self.calls.append(cmd) or False)
        self.assertEqual(self.sync(), 1)
        self.assertFalse(any(isinstance(c, tuple) for c in self.calls))
        self.assertIn("bootstrap-only", self.installed)

    def test_tracked_failure_stops_before_aur_or_prune(self):
        self.patch("install_tracked", lambda names: False)
        self.assertEqual(self.sync(), 1)
        self.assertIn("bootstrap-only", self.installed)
        self.assertNotIn("arbitrary-aur-name", self.installed)

    def test_aur_failure_never_prunes(self):
        self.patch("run", lambda cmd, net=False: False)
        self.assertEqual(self.sync(), 1)
        self.assertIn("bootstrap-only", self.installed)

    def test_success_status_with_missing_package_never_prunes(self):
        self.patch("run", lambda cmd, net=False: True)
        self.assertEqual(self.sync(), 1)
        self.assertIn("bootstrap-only", self.installed)

    def test_dirty_installed_tracked_still_needs_plan_apply(self):
        self.installed["libinput-tools"] = []
        self.patch("local_rebuilds", lambda *args: [("libinput-tools", "patch changed")])
        self.assertEqual(self.sync(), 1)
        self.assertEqual(self.calls, [])

    def test_installed_system_never_prunes_after_tracked_inputs_changed(self):
        self.aur_changes_tracked_inputs()
        self.assertEqual(self.sync(), 1)
        self.assertEqual([c for c in self.calls if isinstance(c, tuple) and c[0] == "tracked"],
                         [("tracked", {"libinput-tools"})])
        self.assertIn("bootstrap-only", self.installed)

    def test_first_install_builds_tracked_again_after_aur_then_prunes(self):
        rebuilt = self.aur_changes_tracked_inputs()
        self.assertEqual(self.sync(first_install=True), 0)
        self.assertEqual(rebuilt, [False, True])
        again = [i for i, c in enumerate(self.calls) if c == ("tracked", {"libinput-tools"})][1]
        aur = self.calls.index(([pkg.BUILD_TOOL, "-i", "arbitrary-aur-name"], True))
        prune = next(i for i, c in enumerate(self.calls) if isinstance(c, str) and "pacman -Rns " in c)
        self.assertLess(aur, again)
        self.assertLess(again, prune)
        self.assertNotIn("bootstrap-only", self.installed)

    def test_continued_first_install_builds_dirty_tracked_first(self):
        self.installed.update({"libinput-tools": [], "repo-program": [], "arbitrary-aur-name": []})
        rebuilt = self.aur_changes_tracked_inputs()
        self.assertEqual(self.sync(first_install=True), 0)
        self.assertEqual(rebuilt, [True])
        self.assertEqual(self.calls[0], ("tracked", {"libinput-tools"}))

    def test_failed_tracked_rebuild_stops_the_installation(self):
        self.installed.update({"libinput-tools": [], "repo-program": [], "arbitrary-aur-name": []})
        self.patch("local_rebuilds", lambda *args: [("libinput-tools", "host libraries or build tools changed")])
        self.patch("install_tracked", lambda names: (_ for _ in ()).throw(RuntimeError("tests failed")))
        self.assertEqual(self.sync(first_install=True), 1)
        self.assertIn("bootstrap-only", self.installed)

    def test_only_the_installer_asks_for_first_install(self):
        script = (SOURCE / "make-root.sh").read_text()
        self.assertIn('chroot "$root" /usr/local/bin/locachy-pkg sync --first-install\n', script)
        installer = (SOURCE / "link/locachy-install").read_text()
        self.assertIn('"/usr/local/bin/locachy-pkg", "sync", "--first-install"]', installer)
        hooks = "".join(p.read_text() for p in (SOURCE / "link").glob("*.hook"))
        self.assertNotIn("--first-install", hooks)
        with mock.patch.object(pkg, "cmd_sync", lambda first=False: first), \
                mock.patch.object(sys, "argv", ["locachy-pkg", "sync"]), self.assertRaises(SystemExit) as end:
            pkg.main()
        self.assertIs(end.exception.code, False)

    def test_first_tracked_install_locks_existing_and_missing_outputs(self):
        self.installed["labwc-locachy"] = []
        selected = []
        snapshot = []
        installed = []
        self.patch("repo_updates", lambda: self.calls.append("private-check"))
        self.patch("checked_local_candidates", lambda recipes, names:
                   selected.append(set(names)) or ({"libinput-tools": {"recipe_dir": "candidate"}}, {"libinput-tools": "pin"}))
        self.patch("transaction_host_hash", lambda: "host-before")
        transaction = types.SimpleNamespace(snapshot=lambda *args, **kwargs: snapshot.append((args, kwargs)) or "reference")
        self.patch("helper_module", lambda name: transaction)
        self.patch("install_prepared_host", lambda reference, recipes: installed.append((reference, recipes)) or True)
        self.assertTrue(INSTALL_TRACKED({"libinput-tools"}))
        self.assertIn("libinput-tools", selected[0])
        self.assertIn("labwc-locachy", selected[0])
        self.assertEqual(snapshot[0][0][3], "host-before")
        self.assertEqual(snapshot[0][1], {"test_source": pkg.SRC})
        self.assertEqual(installed, [("reference", self.baseline)])

    def test_pacman_omitted_reason_is_explicit(self):
        db = self.root / "db"
        for name, reason in (("leaf", ""), ("dependency", "%REASON%\n1\n\n")):
            entry = db / (name + "-1-1")
            entry.mkdir(parents=True)
            (entry / "desc").write_text("%NAME%\n" + name + "\n\n" + reason)
        packages, providers, explicit = LOAD_INSTALLED(str(db))
        self.assertEqual(set(packages), {"leaf", "dependency"})
        self.assertEqual(explicit, {"leaf"})

    def test_root_bootstrap_uses_the_normal_reconciler_before_make(self):
        script = (SOURCE / "make-root.sh").read_text()
        self.assertNotIn("*-git|", script)
        self.assertNotIn('done < "$here/packages"', script)
        self.assertNotIn("pacman -R --noconfirm labwc", script)
        self.assertIn("export LC_ALL=C.UTF-8", script)
        self.assertIn('gpgconf --homedir /var/lib/locachy/builder/home/.gnupg --kill all', script)
        self.assertIn('-type s -delete', script)
        helpers = script.index('chroot "$root" sh /usr/local/src/locachy/link/install-pkg-tools')
        sync = script.index('chroot "$root" /usr/local/bin/locachy-pkg sync')
        check = script.index('chroot "$root" /usr/local/bin/locachy-pkg status')
        make = script.index('chroot "$root" /usr/local/src/locachy/make.sh --boot')
        archive = script.index('bsdtar --numeric-owner')
        self.assertLess(helpers, sync)
        self.assertLess(sync, check)
        self.assertLess(check, make)
        self.assertLess(make, archive)


class Manifest(unittest.TestCase):
    """install and remove on a real manifest file; sync itself is doubled."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pkg-manifest-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.manifest = Path(self.tmp.name) / "packages"
        self.manifest.write_text("# leaves\nbase\n")
        self.synced = []
        load = pkg.load_manifest  # its default path is bound at definition time
        self.unknown = []
        for name, value in (("MANIFEST", str(self.manifest)), ("load_manifest", lambda path=None: load(str(self.manifest))),
                            ("cmd_sync", lambda: self.synced.append(load(str(self.manifest))) or 0),
                            ("unknown_packages", lambda names: [n for n in names if n in self.unknown])):
            p = mock.patch.object(pkg, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.load = load

    def call(self, function, names):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            return function(names), err.getvalue()

    def test_names_typed_together_are_separate_leaves(self):
        self.assertEqual(self.call(pkg.cmd_install, ["libraw chmlib"])[0], 0)
        self.assertEqual(self.manifest.read_text(), "# leaves\nbase\nlibraw\nchmlib\n")
        self.assertEqual(self.synced, [["base", "chmlib", "libraw"]])

    def test_an_invalid_name_writes_nothing(self):
        for names in (["libraw -rf"], ["good", "bad/name"], [".hidden"], ["a;b"]):
            status, err = self.call(pkg.cmd_install, names)
            self.assertEqual(status, 1, names)
            self.assertIn("not a package name", err)
        self.assertEqual(self.manifest.read_text(), "# leaves\nbase\n")
        self.assertEqual(self.synced, [])

    def test_a_name_nothing_has_writes_nothing(self):
        self.unknown = ["xorg-host"]
        status, err = self.call(pkg.cmd_install, ["libraw xorg-host"])
        self.assertEqual(status, 1)
        self.assertIn("no repository or the AUR has xorg-host; nothing registered", err)
        self.assertEqual(self.manifest.read_text(), "# leaves\nbase\n")
        self.assertEqual(self.synced, [])

    def test_a_failed_lookup_does_not_register_or_sync(self):
        with mock.patch.object(pkg, "unknown_packages", side_effect=RuntimeError("the AUR did not answer")):
            status, err = self.call(pkg.cmd_install, ["libraw chmlib"])
        self.assertEqual(status, 1)
        self.assertIn("nothing registered", err)
        self.assertEqual(self.manifest.read_text(), "# leaves\nbase\n")
        self.assertEqual(self.synced, [])

    def test_a_line_with_several_names_reads_as_each_and_loses_only_the_removed(self):
        self.manifest.write_text("# leaves\nbase\nlibraw chmlib  # typed together\n")
        self.assertEqual(self.load(str(self.manifest)), ["base", "chmlib", "libraw"])
        self.assertEqual(pkg.declared_missing({"base": "1", "libraw": "1"}), ["chmlib"])
        self.assertEqual(self.call(pkg.cmd_remove, ["libraw"])[0], 0)
        self.assertEqual(self.load(str(self.manifest)), ["base", "chmlib"])
        self.assertIn("# typed together\nchmlib\n", self.manifest.read_text())
        self.assertEqual(self.call(pkg.cmd_remove, ["chmlib"])[0], 0)
        self.assertEqual(self.load(str(self.manifest)), ["base"])
        repos = (SOURCE / "link/locachy-repos").read_text()
        self.assertIn('leaves.extend(line.split("#", 1)[0].split())', repos)


class Lookup(unittest.TestCase):
    """Where a name to declare is looked up: local recipes, then the
    repositories (pacman -Sp), then the AUR for what is left."""
    def test_local_then_repositories_then_the_aur(self):
        asked = []
        def run(cmd, **kwargs):
            asked.append(cmd[-1])
            return subprocess.CompletedProcess(cmd, 0 if cmd[-1] == "libraw" else 1)
        with mock.patch.object(pkg, "local_names", lambda: {"labwc-locachy"}), \
                mock.patch.object(pkg.subprocess, "run", run), \
                mock.patch.object(pkg, "aur_info", return_value={"bluetui": ("1", "")}) as lookup:
            self.assertEqual(pkg.unknown_packages(["labwc-locachy", "libraw", "bluetui", "xorg-host"]), ["xorg-host"])
        self.assertEqual(asked, ["libraw", "bluetui", "xorg-host"])
        lookup.assert_called_once_with(["bluetui", "xorg-host"])

    def test_the_aur_not_answering_is_an_error_not_a_yes(self):
        def down(names):
            raise RuntimeError("the AUR did not answer")
        with mock.patch.object(pkg, "local_names", lambda: set()), \
                mock.patch.object(pkg.subprocess, "run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 1)), \
                mock.patch.object(pkg, "aur_info", down):
            with self.assertRaises(RuntimeError):
                pkg.unknown_packages(["xorg-host"])


if __name__ == "__main__":
    unittest.main()
