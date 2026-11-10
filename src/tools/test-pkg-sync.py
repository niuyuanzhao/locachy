#!/usr/bin/env python3
"""Fresh-root reconciliation order and failure boundaries; no real pacman.
  python3 -B tools/test-pkg-sync.py SCRATCH_PARENT
"""
import contextlib
import fcntl
import importlib.machinery
import importlib.util
import io
import json
import os
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
LOCAL_RECIPES = pkg.local_recipes
LOCAL_REBUILDS = pkg.local_rebuilds
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
        self.patch("local_recipes", lambda build=True: self.baseline)
        self.patch("tracked_recipes", lambda recipes: recipes)
        self.patch("local_names", lambda recipes=None: set(self.baseline))
        self.patch("local_packages", lambda db: {n: "1" for n in self.installed})
        self.patch("local_rebuilds", lambda *args: [])
        self.patch("load_installed_db", lambda: (dict(self.installed), {n: [n] for n in self.installed},
                                                set(self.installed)))
        self.patch("install_tracked", self.tracked)
        self.patch("needed_vulkan_drivers", lambda repo, aur: [])
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

    def test_dirty_installed_tracked_waits_for_the_update(self):
        """Installing or removing others never waits for a validated
        rebuild; sync never builds a tracked package itself and says the
        update has it."""
        self.installed["libinput-tools"] = []
        self.patch("local_rebuilds", lambda *args: [("libinput-tools", "host libraries or build tools changed")])
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(pkg.os.path, "isfile", return_value=True):
            self.assertEqual(pkg.cmd_sync(), 0)
        self.assertFalse(any(isinstance(c, tuple) and c[0] == "tracked" for c in self.calls))
        self.assertFalse(any(isinstance(c, tuple) and "libinput-tools" in c[0] for c in self.calls))
        self.assertIn("repo-program", self.installed)
        self.assertIn("arbitrary-aur-name", self.installed)
        self.assertNotIn("bootstrap-only", self.installed)
        self.assertIn("plan, then apply", out.getvalue())
        self.assertIn("libinput-tools", out.getvalue())

    def test_stale_promoted_tracked_recipe_waits_for_the_update(self):
        """A source update changed a tracked package's patch: its promoted
        build no longer matches. A build refuses it (fail closed); sync,
        which builds none of them, installs and prunes the others and says
        the update has it."""
        self.installed["libinput-tools"] = []
        pkg.STALE_TRACKED.clear()
        self.addCleanup(pkg.STALE_TRACKED.clear)

        class UpstreamError(RuntimeError):
            pass

        def effective(name, baseline, records):
            raise UpstreamError("local recipe or patch changed since Check")
        upstream = types.SimpleNamespace(UpstreamError=UpstreamError, effective_recipe=effective)
        self.patch("helper_module", lambda name: upstream)
        self.patch("local_recipes", LOCAL_RECIPES)
        self.patch("local_rebuilds", LOCAL_REBUILDS)
        with self.assertRaises(UpstreamError):
            pkg.local_recipes()
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(pkg.os.path, "isfile", return_value=True):
            self.assertEqual(pkg.cmd_sync(), 0)
        self.assertFalse(any(isinstance(c, tuple) and c[0] == "tracked" for c in self.calls))
        self.assertFalse(any(isinstance(c, tuple) and "libinput-tools" in c[0] for c in self.calls))
        self.assertIn("repo-program", self.installed)
        self.assertIn("arbitrary-aur-name", self.installed)
        self.assertNotIn("bootstrap-only", self.installed)
        self.assertIn("libinput-tools", out.getvalue())
        self.assertEqual(pkg.STALE_TRACKED, {"libinput-tools": "local recipe or patch changed since Check"})

    def test_installed_system_leaves_changed_tracked_inputs_to_the_update(self):
        self.aur_changes_tracked_inputs()
        self.assertEqual(self.sync(), 0)
        # Its first install, the validated way; never built again by sync.
        self.assertEqual([c for c in self.calls if isinstance(c, tuple) and c[0] == "tracked"],
                         [("tracked", {"libinput-tools"})])
        self.assertNotIn("bootstrap-only", self.installed)

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
                mock.patch.object(pkg, "job_route", lambda action, args: None), \
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

    def test_the_machines_vulkan_driver_comes_before_what_needs_one(self):
        self.patch("needed_vulkan_drivers", lambda repo, aur: ["vulkan-intel"] if "arbitrary-aur-name" in aur else [])
        self.assertEqual(self.sync(), 0)
        driver = self.calls.index("pacman -S --needed --noconfirm --asdeps vulkan-intel")
        repo = next(i for i, c in enumerate(self.calls) if isinstance(c, str) and "--assume-installed initramfs" in c)
        aur = self.calls.index(([pkg.BUILD_TOOL, "-i", "arbitrary-aur-name"], True))
        self.assertLess(driver, repo)
        self.assertLess(driver, aur)

    def test_a_failed_vulkan_driver_stops_before_the_build(self):
        self.patch("needed_vulkan_drivers", lambda repo, aur: ["vulkan-intel"])
        self.patch("run_pacman", lambda cmd: self.calls.append(cmd) or "vulkan-intel" not in cmd)
        self.assertEqual(self.sync(), 1)
        self.assertFalse(any(isinstance(c, tuple) for c in self.calls))
        self.assertIn("bootstrap-only", self.installed)

    def test_pacman_omitted_reason_is_explicit(self):
        db = self.root / "db"
        for name, reason in (("leaf", ""), ("dependency", "%REASON%\n1\n\n")):
            entry = db / (name + "-1-1")
            entry.mkdir(parents=True)
            (entry / "desc").write_text("%NAME%\n" + name + "\n\n" + reason)
        packages, providers, explicit = LOAD_INSTALLED(str(db))
        self.assertEqual(set(packages), {"leaf", "dependency"})
        self.assertEqual(explicit, {"leaf"})

    def test_removal_installs_nothing(self):
        """Removing a module (settings' Chinese Pinyin) never installs a leaf
        that is declared but not installed (the CJK fonts' noto-fonts-cjk,
        declared by an update and not installed yet): it unregisters, removes
        what no installed leaf needs, builds nothing, and says what stays
        missing. A sync or an update's host step installs that."""
        self.installed.update({"rime-frost-git": [], "librime": []})
        self.leaves = ["base", "librime", "rime-frost-git", "noto-fonts-cjk"]
        self.patch("manifest_lines", lambda: [name + "\n" for name in self.leaves])
        self.patch("write_manifest", lambda lines: setattr(self, "leaves", sorted(
            name for line in lines for name in pkg.line_names(line))))
        self.patch("run", lambda cmd, net=False: self.calls.append((cmd, net)) or True)

        def pacman(command):
            self.calls.append(command)
            args = shlex.split(command)
            if "-Rns" in args:
                for name in args[args.index("--noconfirm") + 1:]:
                    self.installed.pop(name, None)
            return True
        self.patch("run_pacman", pacman)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(pkg.os.path, "isfile", return_value=True):
            status = pkg.cmd_remove(["rime-frost-git"])
        self.assertEqual(status, 0)
        self.assertEqual(self.leaves, ["base", "librime", "noto-fonts-cjk"])
        self.assertFalse(any(isinstance(c, str) and "pacman -S " in c for c in self.calls), self.calls)
        self.assertFalse(any(isinstance(c, list) and "-Sp" in c for c in self.calls), self.calls)  # not even looked up
        self.assertFalse(any(isinstance(c, tuple) for c in self.calls), self.calls)  # no build, tracked or AUR
        self.assertNotIn("rime-frost-git", self.installed)
        self.assertNotIn("bootstrap-only", self.installed)
        self.assertNotIn("noto-fonts-cjk", self.installed)
        self.assertIn("librime", self.installed)
        self.assertIn("Declared, not installed", out.getvalue())
        self.assertIn("noto-fonts-cjk", out.getvalue())
        # A sync (or an install) still installs what is declared.
        self.calls.clear()
        self.assertEqual(self.sync(), 1)  # the double installs only repo-program: noto-fonts-cjk stays missing
        self.assertTrue(any(isinstance(c, tuple) and "noto-fonts-cjk" in c[0] for c in self.calls), self.calls)

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


class VulkanDriver(unittest.TestCase):
    """vulkan-driver for what needs it: this machine's GPUs' drivers, not the
    first provider in the repositories' order."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pkg-vulkan-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.drm = Path(self.tmp.name) / "drm"

    def card(self, name, driver):
        device = self.drm.parent / (self.drm.name + "-devices") / name
        (device / "drivers" / driver).mkdir(parents=True)
        (self.drm / name).mkdir(parents=True)
        (self.drm / name / "device").symlink_to(device)
        (device / "driver").symlink_to(device / "drivers" / driver)

    def test_each_gpus_driver_once_else_the_software_one(self):
        self.card("card0", "i915")
        self.card("card0-eDP-1", "i915")  # a connector
        self.card("card1", "nouveau")
        self.card("card2", "xe")
        self.card("card3", "evdi")
        self.assertEqual(pkg.gpu_vulkan_drivers(str(self.drm)), ["vulkan-intel", "vulkan-nouveau"])
        other = Path(self.tmp.name) / "other"
        other.mkdir()
        self.drm = other
        self.card("card0", "bochs-drm")
        self.assertEqual(pkg.gpu_vulkan_drivers(str(other)), ["vulkan-swrast"])

    def needed(self, satisfied, repo_depends, aur_depends, repo=("repo-program",), aur=("aur-program",)):
        def run(cmd, **kwargs):
            if cmd[:2] == ["pacman", "-T"]:
                return subprocess.CompletedProcess(cmd, 0 if satisfied else 127)
            return subprocess.CompletedProcess(cmd, 0, stdout="Name            : x\nDepends On      : " + repo_depends + "\n")
        with mock.patch.object(pkg.subprocess, "run", run), \
                mock.patch.object(pkg, "aur_results", lambda names: [{"Name": n, "Depends": aur_depends} for n in names]), \
                mock.patch.object(pkg, "gpu_vulkan_drivers", lambda: ["vulkan-intel"]):
            return pkg.needed_vulkan_drivers(list(repo), list(aur))

    def test_only_when_needed_and_none_is_installed(self):
        self.assertEqual(self.needed(False, "None", ["gtk3", "vulkan-driver"]), ["vulkan-intel"])
        self.assertEqual(self.needed(False, "glibc  vulkan-driver>=1", [], aur=()), ["vulkan-intel"])
        self.assertEqual(self.needed(True, "None", ["vulkan-driver"]), [])
        self.assertEqual(self.needed(False, "glibc", ["gtk3", "vulkan-icd-loader"]), [])
        self.assertEqual(self.needed(False, "None", [], repo=(), aur=()), [])

    def test_the_aur_not_answering_leaves_it_to_the_build(self):
        def down(names):
            raise RuntimeError("the AUR did not answer")
        with mock.patch.object(pkg.subprocess, "run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 127)), \
                mock.patch.object(pkg, "aur_results", down):
            self.assertEqual(pkg.needed_vulkan_drivers([], ["aur-program"]), [])


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
                            ("cmd_sync", lambda **how: self.synced.append(load(str(self.manifest))) or 0),
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


class RequirementRemoval(unittest.TestCase):
    """Removing Chinese Pinyin leaves no pending download of what it alone
    required: the CJK fonts' module, declared by an update as ime-zh's
    requirement (declare-packages) and not installed, is unregistered with it;
    installed, or chosen on its own, it stays. The next update declares it
    again only with Pinyin. modules.json and declare-packages are the tree's."""
    PINYIN = ["rime-frost-git", "rime-pinyin-simp", "rime-wanxiang-gram-zh-hans"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pkg-requirement-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "packages"
        self.record = self.root / "declared-packages.json"
        self.manifest.write_text("# leaves\nbase\nlibrime\nwords\n" + "".join(p + "\n" for p in self.PINYIN))
        self.installed = {"base", "librime", "words", *self.PINYIN}
        self.synced = []
        load = pkg.load_manifest  # its default path is bound at definition time
        self.load = lambda: load(str(self.manifest))
        loader = importlib.machinery.SourceFileLoader("declare_for_sync", str(SOURCE / "link/declare-packages"))
        self.declare = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(self.declare)
        for module, name, value in (
                (pkg, "SRC", str(SOURCE)), (pkg, "MANIFEST", str(self.manifest)), (pkg, "load_manifest", lambda path=None: self.load()),
                (pkg, "cmd_sync", lambda **how: self.synced.append(how) or 0),
                (pkg, "unknown_packages", lambda names: []),
                (pkg, "load_installed_db", lambda db=None: ({n: [] for n in self.installed}, {}, set(self.installed))),
                (self.declare, "fontconfig_has", lambda lang: False)):
            p = mock.patch.object(module, name, value)
            p.start()
            self.addCleanup(p.stop)

    def update(self, record_requires=True):
        """An update's declare-packages run; without record_requires, the one
        that first saw ime-zh's requirements (its record from before them)."""
        current = self.declare.definitions(SOURCE)
        if not record_requires:
            current = {k: v for k, v in current.items() if k != "requires"}
        if not self.record.exists() or not record_requires:
            self.record.write_text(json.dumps(current))
        with contextlib.redirect_stdout(io.StringIO()):
            return self.declare.declare(str(SOURCE), str(self.manifest), str(self.record))

    def call(self, function, names):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()):
            return function(names), out.getvalue()

    def test_declared_not_installed_fonts_go_with_pinyin(self):
        groups = self.update(record_requires=False)
        self.assertEqual(groups, [("module cjk-fonts (required by ime-zh)", ["noto-fonts-cjk"])])
        self.assertIn("noto-fonts-cjk", self.load())
        status, out = self.call(pkg.cmd_remove, [" ".join(self.PINYIN)])
        self.assertEqual(status, 0)
        self.assertEqual(self.load(), ["base", "librime", "words"])  # Rime (installed) stays
        self.assertEqual(self.synced, [{"install": False}])  # nothing installed
        self.assertIn("Unregistering module cjk-fonts", out)
        # The next update, and its sync: no CJK fonts while Pinyin is absent.
        self.assertEqual(self.update(), [])
        self.assertNotIn("noto-fonts-cjk", self.load())

    def test_installed_fonts_stay(self):
        self.update(record_requires=False)
        self.installed.add("noto-fonts-cjk")
        self.assertEqual(self.call(pkg.cmd_remove, [" ".join(self.PINYIN)])[0], 0)
        self.assertIn("noto-fonts-cjk", self.load())

    def test_fonts_chosen_on_their_own_stay(self):
        # A settings row's install after the update declared it as a requirement.
        self.update(record_requires=False)
        self.assertEqual(self.call(pkg.cmd_install, ["noto-fonts-cjk"])[0], 0)
        self.assertNotIn("(required by ime-zh)\nnoto-fonts-cjk\nnoto-fonts-cjk", self.manifest.read_text())
        self.assertEqual(self.call(pkg.cmd_remove, [" ".join(self.PINYIN)])[0], 0)
        self.assertIn("noto-fonts-cjk", self.load())
        # Declared on its own from the start (the installer's list, a plain line).
        self.manifest.write_text("# leaves\nbase\nnoto-fonts-cjk\nlibrime\nwords\n" +
                                 "".join(p + "\n" for p in self.PINYIN))
        self.assertEqual(self.call(pkg.cmd_remove, [" ".join(self.PINYIN)])[0], 0)
        self.assertIn("noto-fonts-cjk", self.load())

    def test_a_user_install_after_a_requirement_group_is_its_own(self):
        self.update(record_requires=False)
        self.assertEqual(self.call(pkg.cmd_install, ["tor"])[0], 0)
        self.assertTrue(self.manifest.read_text().endswith("noto-fonts-cjk\n\ntor\n"))
        self.assertEqual(pkg.required_only(pkg.manifest_lines()), {"noto-fonts-cjk"})


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


class JobRoute(unittest.TestCase):
    """Online, every host mutation is a fixed system job; nothing changes the
    host in the calling process, and forms without a job are refused."""

    def route(self, argv, euid=0):
        calls = []
        jobs = types.SimpleNamespace(
            execution_mode=lambda: "queue",
            submit=lambda kind, args=(), secret=None, wait=True, startup_wait=0: calls.append((kind, list(args), wait)) or 0)
        with mock.patch.object(pkg, "helper_module", lambda name: jobs), \
                mock.patch.object(pkg.os, "geteuid", lambda: euid), \
                mock.patch.object(pkg, "acquire_pkg_lock", lambda: self.fail("took the domain in the caller")), \
                mock.patch.object(sys, "argv", ["locachy-pkg"] + argv), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as end:
            pkg.main()
        return end.exception.code, calls

    def test_each_mutation_is_its_fixed_job_and_waits_for_it(self):
        for argv, job in ((["install", "gimp"], ("pkg-install", ["gimp"])),
                          (["remove", "gimp"], ("pkg-remove", ["gimp"])),
                          (["sync"], ("pkg-sync", [])),
                          (["plan"], ("pkg-plan", [])),
                          (["apply", "host"], ("pkg-apply", ["host"])),
                          (["kernel", "select", "linux-lts"], ("kernel-select", ["linux-lts"])),
                          (["kernel", "headers", "on"], ("kernel-headers", ["on"]))):
            with self.subTest(argv=argv):
                code, calls = self.route(argv)
                self.assertEqual(code, 0)
                self.assertEqual(calls, [job + (True,)])

    def test_forms_without_a_job_and_non_root_change_nothing(self):
        for argv in (["sync", "--first-install"], ["apply", "--after-source"], ["source-built", "/x"]):
            with self.subTest(argv=argv):
                code, calls = self.route(argv)
                self.assertNotIn(code, (0, None))
                self.assertEqual(calls, [])
        code, calls = self.route(["install", "gimp"], euid=1000)
        self.assertIn("must run as root", str(code))
        self.assertEqual(calls, [])


class DomainInheritance(unittest.TestCase):
    """A nested locachy-pkg reuses the domain its job holds through the
    inherited open file description; an independent process is refused."""

    CHILD = ("import sys; sys.path.insert(0, sys.argv[1]);"
             "import importlib.machinery, importlib.util;"
             "loader = importlib.machinery.SourceFileLoader('pkg', sys.argv[2]);"
             "spec = importlib.util.spec_from_loader('pkg', loader); m = importlib.util.module_from_spec(spec);"
             "loader.exec_module(m); m.DOMAIN_LOCK_PATH = sys.argv[3]; m.domain_applies = lambda: True;"
             "m.domain_mode = lambda: sys.argv[4];"
             "m.acquire_pkg_lock(); print('domain held')")

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "the inherited descriptor is found through /proc")
    def test_the_builder_drops_an_inherited_domain_before_it_starts_anything(self):
        text = (SOURCE / "link/locachy-build").read_text()
        start = text.index("for fdpath in /proc/self/fd/*; do")
        loop = text[start:text.index("done\n", start) + len("done\n")]
        with tempfile.TemporaryDirectory(prefix="build-domain-", dir=SCRATCH) as tmp:
            lock = os.path.join(tmp, "pkg.lock")
            holder = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
            fcntl.flock(holder, fcntl.LOCK_EX)
            script = ("set -euo pipefail\n" + loop.replace("/run/locachy/pkg.lock", lock) +
                      f'for p in /proc/self/fd/*; do if [ "$p" -ef "{lock}" ]; then echo LEAKED; fi; done\n'
                      "echo CHECKED\n")
            result = subprocess.run(["bash", "-c", script], pass_fds=(holder,), capture_output=True, text=True,
                                    timeout=30)
            os.close(holder)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CHECKED", result.stdout)
        self.assertNotIn("LEAKED", result.stdout)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "the inherited descriptor is found through /proc")
    def test_a_system_job_never_takes_the_domain_itself(self):
        loader = importlib.machinery.SourceFileLoader("locachy_transaction",
                                                      str(SOURCE / "link/locachy_transaction.py"))
        spec = importlib.util.spec_from_loader("locachy_transaction", loader)
        tx = importlib.util.module_from_spec(spec)
        loader.exec_module(tx)
        with tempfile.TemporaryDirectory(prefix="job-domain-", dir=SCRATCH) as tmp:
            lock = os.path.join(tmp, "pkg.lock")
            with self.assertRaisesRegex(RuntimeError, "does not hold"):
                tx.hold_domain(lock, inside_job=True)
            self.assertFalse(os.path.exists(lock))  # refused before any lock file is made
            holder = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
            fcntl.flock(holder, fcntl.LOCK_EX)
            os.set_inheritable(holder, True)  # what the jobs runner passes down
            try:
                self.assertGreaterEqual(tx.hold_domain(lock, inside_job=True), 0)
            finally:
                os.close(holder)

    def test_production_code_has_no_runtime_override_of_the_domain(self):
        for name in ("link/locachy-pkg", "link/locachy-repos", "link/locachy_transaction.py",
                     "link/locachy-pacman-guard"):
            text = (SOURCE / name).read_text()
            for variable in ("LOCACHY_PKG_LOCK", "JOBS_PKG_LOCK"):
                self.assertNotIn(variable, text, name)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "the inherited descriptor is found through /proc")
    def test_inherited_domain_is_reused_and_an_independent_one_is_refused(self):
        with tempfile.TemporaryDirectory(prefix="domain-inherit-", dir=SCRATCH) as tmp:
            lock = os.path.join(tmp, "pkg.lock")
            holder = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
            fcntl.flock(holder, fcntl.LOCK_EX)
            command = [sys.executable, "-I", "-B", "-c", self.CHILD, str(SOURCE / "link"),
                       str(SOURCE / "link/locachy-pkg"), lock, "offline"]
            inherited = subprocess.run(command, pass_fds=(holder,), capture_output=True, text=True, timeout=30)
            self.assertEqual(inherited.returncode, 0, inherited.stderr)
            self.assertIn("domain held", inherited.stdout)
            independent = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(independent.returncode, 0)
            self.assertIn("another package or repository operation is running", independent.stderr)
            os.close(holder)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "the inherited descriptor is found through /proc")
    def test_the_tool_in_a_job_reuses_the_inherited_domain_and_never_makes_one(self):
        """The tool's own job branch (acquire_pkg_lock, mode 'job'), run as a
        real process: refused without the runner's descriptor, with no lock file
        made; reused with it."""
        with tempfile.TemporaryDirectory(prefix="job-tool-", dir=SCRATCH) as tmp:
            lock = os.path.join(tmp, "pkg.lock")
            command = [sys.executable, "-I", "-B", "-c", self.CHILD, str(SOURCE / "link"),
                       str(SOURCE / "link/locachy-pkg"), lock, "job"]
            refused = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("does not hold", refused.stderr)
            self.assertFalse(os.path.exists(lock))
            holder = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
            fcntl.flock(holder, fcntl.LOCK_EX)
            try:
                inherited = subprocess.run(command, pass_fds=(holder,), capture_output=True, text=True,
                                           timeout=30)
                self.assertEqual(inherited.returncode, 0, inherited.stderr)
                self.assertIn("domain held", inherited.stdout)
            finally:
                os.close(holder)


if __name__ == "__main__":
    unittest.main()
