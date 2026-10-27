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

    def sync(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(pkg.os.path, "isfile", return_value=True):
            return pkg.cmd_sync()

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


if __name__ == "__main__":
    unittest.main()
