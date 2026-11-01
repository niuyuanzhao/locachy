#!/usr/bin/env python3
"""The app presets' catalog (link/locachy_apps.py) and the updater's apps
target (locachy-pkg), with local git repositories as the apps repository: no
network, no root, no package manager.
  python3 -B tools/test-apps-catalog.py SCRATCH_PARENT
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(module)
    return module


apps = load("locachy_apps", SOURCE / "link/locachy_apps.py")
pkg = load("apps_package_tool", SOURCE / "link/locachy-pkg")
HELPER = SOURCE / "link/locachy_apps.py"
EDGE = ('# LOCachy preset: Microsoft Edge, on the network.\npackage="microsoft-edge-stable-bin"\nextras=""\n'
        'command="/usr/bin/microsoft-edge-stable"\narguments="--ozone-platform=wayland"\nnetwork="1"\n'
        'camera="1"\ndirectories=""\n')
GIMP = '# LOCachy preset: GIMP.\npackage="gimp"\nextras=""\ncommand="/usr/bin/gimp"\narguments=""\nnetwork="0"\n'
MPV = '# LOCachy preset: mpv.\npackage="mpv"\ncommand="/usr/bin/mpv"\nnetwork="1"\n'
FOOT = 'host="1"\npackage="foot"\ncommand="/usr/bin/foot"\ndesktop="foot.desktop"\n'
BUILDS = ("make.sh", "locachy-boot", "pacman", "locachy-build", "checkupdates")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="apps-catalog-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.apps_dir = self.root / "apps"
        self.conf = self.root / "containers"
        self.conf.mkdir()
        self.signers = self.root / "trusted-signers"
        env = mock.patch.dict(os.environ, {"HOME": str(self.root), "GIT_CONFIG_NOSYSTEM": "1",
                                           "GIT_AUTHOR_NAME": "test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                                           "GIT_COMMITTER_NAME": "test",
                                           "GIT_COMMITTER_EMAIL": "test@example.invalid"})
        env.start()
        self.addCleanup(env.stop)
        for name in ("LOCACHY_APPS_REPO", "LOCACHY_APPS_BRANCH", "LOCACHY_APPS_CATALOG"):
            os.environ.pop(name, None)
        self.remote = self.root / "remote"
        self.git("init", "-q", "-b", "main", str(self.remote))
        self.c1 = self.commit({"FORMAT": "1\n", "apps/edge.conf": EDGE, "apps/gimp.conf": GIMP,
                               "apps/mpv.conf": MPV}, "First presets")
        self.url = f"file://{self.remote}"

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()

    def commit(self, files, message):
        for name, text in files.items():
            path = self.remote / name
            if text is None:
                path.unlink()
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.git("add", "-A", cwd=self.remote)
        self.git("commit", "-q", "-m", message, cwd=self.remote)
        return self.git("rev-parse", "HEAD", cwd=self.remote)

    def catalog(self):
        return self.apps_dir / "catalog"

    def install(self):
        """The remote's head, checked and applied: (commit, changed)."""
        found = apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        return found["commit"], apps.apply(str(self.apps_dir), found["commit"], str(self.conf),
                                           str(self.signers), self.url, "main")


class Validation(Base):
    def tree(self, files, fmt="1\n"):
        tree = Path(tempfile.mkdtemp(dir=self.root))
        if fmt is not None:
            (tree / "FORMAT").write_text(fmt)
        (tree / "apps").mkdir()
        for name, data in files.items():
            (tree / "apps" / name).write_bytes(data if isinstance(data, bytes) else data.encode())
        return tree

    def refused(self, said, files=None, fmt="1\n", tree=None):
        tree = tree or self.tree({"edge.conf": EDGE} if files is None else files, fmt)
        with self.assertRaises(apps.CatalogError) as caught:
            apps.validate_tree(str(tree))
        self.assertIn(said, str(caught.exception))
        # The command says the same and fails: the whole catalog refused.
        p = subprocess.run([sys.executable, "-I", str(HELPER), "validate", str(tree)], capture_output=True, text=True)
        self.assertEqual((p.returncode, p.stdout), (1, ""))
        self.assertIn(said, p.stderr)

    def test_each_rule(self):
        fmt, presets = apps.validate_tree(str(self.tree({"edge.conf": EDGE, "gimp.conf": GIMP})))
        self.assertEqual((fmt, sorted(presets)), ("1", ["edge", "gimp"]))
        p = subprocess.run([sys.executable, "-I", str(HELPER), "validate", str(self.tree({"edge.conf": EDGE}))],
                           capture_output=True, text=True)
        self.assertEqual((p.returncode, p.stdout), (0, "OK 1 app presets, format 1\n"))
        self.refused("not compatible: format 2", fmt="2\n")
        self.refused("not compatible: format 1", fmt=" 1\n")
        self.refused("no FORMAT", fmt=None)
        tree = self.tree({})
        self.refused("no presets in apps", tree=tree)
        shutil.rmtree(tree / "apps")
        self.refused("no apps directory", tree=tree)
        (tree / "apps").symlink_to(self.remote / "apps")
        self.refused("no apps directory", tree=tree)
        for name in ("Edge.conf", "edge.txt", ".conf", "-x.conf", "a" * 64 + ".conf"):
            self.refused("not a preset's name", {"edge.conf": EDGE, name: GIMP})
        tree = self.tree({"edge.conf": EDGE})
        (tree / "apps/link.conf").symlink_to(tree / "apps/edge.conf")
        self.refused("link.conf: not a regular file", tree=tree)
        tree = self.tree({"edge.conf": EDGE})
        (tree / "apps/dir.conf").mkdir()
        self.refused("dir.conf: not a regular file", tree=tree)
        self.refused("more than 16384 bytes", {"edge.conf": EDGE + "#" * 16384 + "\n"})
        self.refused("edge.conf: not UTF-8", {"edge.conf": EDGE.encode() + b"# \xff\n"})
        lines = {"package=edge": 'line 9: not KEY="VALUE"', ' network="1"': 'not KEY="VALUE"',
                 'network="1"\r': 'not KEY="VALUE"', 'Network="1"': 'not KEY="VALUE"',
                 'foo="1"': "unknown key foo", 'packages="a b"': "unknown key packages",
                 'network="0"': "network a second time",
                 'host="1"': "host programs are the system's, never an app preset",
                 'arguments="$(touch x)"': "arguments has a quote, backslash, $, backquote or control character",
                 'arguments="a\\b"': "backslash", 'arguments="`id`"': "backquote",
                 'arguments="a"b"': "quote", 'arguments="a\x01"': "control character"}
        for line, said in lines.items():
            text = EDGE.replace('arguments="--ozone-platform=wayland"\n', "") if line.startswith("arguments") else EDGE
            self.refused(said, {"edge.conf": text + line + "\n"})
        values = {"package": [("two names", "package is not one package name"), ("-x", "package is not")],
                  "extras": [("ok -bad", "extras is not package names")],
                  "command": [("usr/bin/x", "command is not an absolute path"), ("/usr/../bin/x", "command is not"),
                              ("/usr/bin/a b", "command is not"), ("/", "command is not")],
                  "network": [("2", "network is not 0 or 1")], "camera": [("yes", "camera is not 0 or 1")],
                  "directories": [("Documents", "directories is not absolute paths"),
                                  ("/a:/b/../c", "directories is not"), ("/a::/b", "directories is not")]}
        for key, cases in values.items():
            for value, said in cases:
                text = "".join(line + "\n" for line in EDGE.splitlines() if not line.startswith(key + "="))
                self.refused(said, {"edge.conf": text + f'{key}="{value}"\n'})
        self.refused("x11 is not 0 or 1", {"edge.conf": EDGE + 'x11="true"\n'})
        self.refused("desktop is not desktop file names", {"edge.conf": EDGE + 'desktop="edge.desktop edge"\n'})
        apps.validate_tree(str(self.tree({"edge.conf": EDGE.replace(
            'directories=""', 'directories="/home/user/Projects:/srv/x y"') + 'x11="1"\ndesktop="a.desktop b-c.desktop"\n'})))
        self.refused("edge.conf: no package", {"edge.conf": EDGE.replace('package="microsoft-edge-stable-bin"\n', "")})
        self.refused("edge.conf: no command",
                     {"edge.conf": EDGE.replace('command="/usr/bin/microsoft-edge-stable"\n', "")})
        # The first invalid file rejects them all.
        self.refused("a.conf: line 1: unknown key foo", {"a.conf": 'foo="1"\n', "edge.conf": EDGE, "z.conf": "x\n"})

    def test_the_catalog_command_writes_a_new_catalog_only_from_a_valid_tree(self):
        dest = self.root / "catalog"
        tree = self.tree({"edge.conf": EDGE})
        command = [sys.executable, "-I", str(HELPER), "catalog", str(tree), str(dest), "medium",
                   "https://example.invalid/apps.git", "main", "a" * 40]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.assertEqual((dest / "edge.conf").read_text(), EDGE)
        meta = apps.read_meta(str(dest))
        self.assertEqual((meta["origin"], meta["commit"], meta["format"]), ("medium", "a" * 40, "1"))
        self.assertEqual(sorted(apps.read_catalog(str(dest))), ["edge"])
        p = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(p.returncode, 1)  # never over an existing one
        bad = self.tree({"edge.conf": EDGE}, fmt="2\n")
        p = subprocess.run(command[:4] + [str(bad), str(self.root / "other")] + command[6:], capture_output=True,
                           text=True)
        self.assertEqual((p.returncode, p.stderr), (1, "not compatible: format 2\n"))
        self.assertFalse((self.root / "other").exists())
        p = subprocess.run(command[:6] + ["somewhere"] + command[7:], capture_output=True, text=True)
        self.assertEqual(p.returncode, 1)


class Catalog(Base):
    def test_check_and_apply_install_the_catalog(self):
        found = apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        self.assertEqual((found["old"], found["commit"], found["merges"]), ("none", self.c1, []))
        self.assertEqual(found["label"], f"{self.c1[:7]} (First presets)")
        self.assertFalse(self.catalog().exists())  # Check changes nothing
        self.assertEqual(apps.apply(str(self.apps_dir), self.c1, str(self.conf), str(self.signers),
                                    self.url, "main"), [])
        self.assertEqual(sorted(apps.read_catalog(str(self.catalog()))), ["edge", "gimp", "mpv"])
        meta = apps.read_meta(str(self.catalog()))
        self.assertEqual((meta["origin"], meta["repo"], meta["branch"], meta["commit"]),
                         ("repository", self.url, "main", self.c1))
        self.assertIsNone(apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers)))

    def test_incompatible_format_keeps_the_old_catalog(self):
        self.install()
        before = {p.name: p.read_bytes() for p in self.catalog().iterdir()}
        c2 = self.commit({"FORMAT": "2\n", "apps/edge.conf": EDGE.replace("wayland", "x11")}, "Format two")
        with self.assertRaisesRegex(apps.CatalogError, "not compatible: format 2"):
            apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        with self.assertRaisesRegex(apps.CatalogError, "not compatible: format 2"):
            apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main")
        self.assertEqual({p.name: p.read_bytes() for p in self.catalog().iterdir()}, before)

    def test_interrupted_switch_leaves_a_complete_catalog(self):
        self.install()
        before = {p.name: p.read_bytes() for p in self.catalog().iterdir()}
        c2 = self.commit({"apps/edge.conf": EDGE.replace('camera="1"', 'camera="0"'), "apps/mpv.conf": None},
                         "Edge without the cameras, no mpv")
        apps.fetch(str(self.apps_dir), self.url, "main")
        real = apps._exchange

        def stopped(new, current):
            raise OSError("stopped before the switch")
        with mock.patch.object(apps, "_exchange", stopped), self.assertRaises(OSError):
            apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main")
        self.assertEqual({p.name: p.read_bytes() for p in self.catalog().iterdir()}, before)
        self.assertEqual(sorted(apps.read_catalog(str(self.catalog()))), ["edge", "gimp", "mpv"])

        def after(new, current):
            real(new, current)
            raise OSError("stopped after the switch")
        with mock.patch.object(apps, "_exchange", after), self.assertRaises(OSError):
            apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main")
        self.assertEqual(sorted(apps.read_catalog(str(self.catalog()))), ["edge", "gimp"])
        self.assertEqual(apps.read_meta(str(self.catalog()))["commit"], c2)
        self.assertIn('camera="0"', (self.catalog() / "edge.conf").read_text())
        # What was left aside is removed by the next switch.
        self.assertTrue((self.apps_dir / "catalog.new").exists())
        apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main")
        self.assertFalse((self.apps_dir / "catalog.new").exists())
        self.assertEqual(sorted(p.name for p in self.apps_dir.iterdir()), ["baseline", "catalog", "repo.git"])

    def test_remote_advancing_after_check_still_applies_the_checked_commit(self):
        self.install()
        c2 = self.commit({"apps/gimp.conf": GIMP.replace('network="0"', 'network="1"')}, "GIMP on the network")
        found = apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        self.assertEqual(found["commit"], c2)
        self.commit({"apps/gimp.conf": GIMP.replace('extras=""', 'extras="gimp-help-en"')}, "Newer still")
        apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main")
        self.assertEqual(apps.read_meta(str(self.catalog()))["commit"], c2)
        self.assertEqual((self.catalog() / "gimp.conf").read_text(), GIMP.replace('network="0"', 'network="1"'))
        with self.assertRaisesRegex(apps.CatalogError, "check again"):
            apps.apply(str(self.apps_dir), "f" * 40, str(self.conf), str(self.signers), self.url, "main")

    def test_signature_required_when_trusted_signers_is_not_empty(self):
        self.signers.write_text("")
        self.install()  # empty: not required
        self.commit({"apps/gimp.conf": GIMP.replace("GIMP", "GIMP, unsigned")}, "Unsigned")
        self.signers.write_text("test@example.invalid ssh-ed25519 "
                                "AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n")
        with self.assertRaisesRegex(apps.CatalogError, "failed signature verification"):
            apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        if not shutil.which("ssh-keygen"):
            self.skipTest("no ssh-keygen: a signed commit is not made")
        key = self.root / "signing-key"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "test", "-f", str(key)], check=True)
        self.signers.write_text('test@example.invalid namespaces="git" ' + Path(str(key) + ".pub").read_text())
        for name, value in (("gpg.format", "ssh"), ("user.signingkey", str(key)), ("commit.gpgsign", "true")):
            self.git("config", name, value, cwd=self.remote)
        signed = self.commit({"apps/gimp.conf": GIMP.replace("GIMP", "GIMP, signed")}, "Signed")
        found = apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        self.assertEqual(found["commit"], signed)
        apps.apply(str(self.apps_dir), signed, str(self.conf), str(self.signers), self.url, "main")
        self.assertIn("GIMP, signed", (self.catalog() / "gimp.conf").read_text())

    def test_merge_follows_untouched_keys_and_keeps_the_users(self):
        self.install()
        edge = EDGE.replace("# LOCachy preset", "# mine\n# LOCachy preset").replace(
            'arguments="--ozone-platform=wayland"', 'arguments="--ozone-platform=wayland --incognito"')
        configurations = {"edge.conf": edge, "gimp.conf": GIMP, "mpv.conf": MPV,
                          "mine.conf": 'package="hello"\ncommand="/usr/bin/hello"\n',
                          "firefox.conf": 'package="firefox"\ncommand="/usr/bin/firefox"\nnetwork="0"\n',
                          "foot.conf": FOOT}
        for name, text in configurations.items():
            (self.conf / name).write_text(text)
        new_edge = EDGE.replace('arguments="--ozone-platform=wayland"', 'arguments="--enable-features=X"').replace(
            'directories=""\n', 'x11="1"\n')
        c2 = self.commit({"apps/edge.conf": new_edge, "apps/gimp.conf": GIMP.replace('extras=""', 'extras="gimp-help-en"'),
                          "apps/mpv.conf": None,
                          "apps/firefox.conf": 'package="firefox"\ncommand="/usr/bin/firefox"\nnetwork="1"\n',
                          "apps/foot.conf": 'package="foot"\ncommand="/usr/bin/foot"\nnetwork="0"\n'},
                         "Edge on X11, GIMP's help, no mpv")
        found = apps.check(str(self.apps_dir), self.url, "main", str(self.conf), str(self.signers))
        self.assertEqual(found["merges"], [("edge", ["directories", "x11"], "microsoft-edge-stable-bin",
                                            "microsoft-edge-stable-bin"),
                                           ("gimp", ["extras"], "gimp", "gimp gimp-help-en")])
        self.assertEqual(apps.apply(str(self.apps_dir), c2, str(self.conf), str(self.signers), self.url, "main"),
                         [("edge", ["directories", "x11"]), ("gimp", ["extras"])])
        edge_now = (self.conf / "edge.conf").read_text()
        self.assertIn("# mine\n", edge_now)  # comments stay
        self.assertIn('arguments="--ozone-platform=wayland --incognito"', edge_now)  # changed: kept
        self.assertNotIn("directories=", edge_now)  # untouched and gone from the preset: removed
        self.assertTrue(edge_now.endswith('x11="1"\n'))  # new in the preset: added
        self.assertEqual(apps.conf_values(edge_now)["camera"], "1")
        self.assertEqual((self.conf / "gimp.conf").read_text(), GIMP.replace('extras=""', 'extras="gimp-help-en"'))
        for name in ("mpv.conf", "mine.conf", "firefox.conf", "foot.conf"):  # left as they are
            self.assertEqual((self.conf / name).read_text(), configurations[name], name)
        self.assertEqual((self.apps_dir / "baseline/edge.conf").read_text(), new_edge)
        self.assertFalse((self.apps_dir / "baseline/firefox.conf").exists())
        # The baseline is the base from now on: a key the user changed since
        # stays theirs, the preset's other changes follow.
        (self.conf / "edge.conf").write_text(edge_now.replace('x11="1"', 'x11="0"'))
        c3 = self.commit({"apps/edge.conf": new_edge.replace('camera="1"', 'camera="0"')}, "Edge without cameras")
        self.assertEqual(apps.apply(str(self.apps_dir), apps.fetch(str(self.apps_dir), self.url, "main"),
                                    str(self.conf), str(self.signers), self.url, "main"), [("edge", ["camera"])])
        self.assertEqual(apps.read_meta(str(self.catalog()))["commit"], c3)
        values = apps.conf_values((self.conf / "edge.conf").read_text())
        self.assertEqual((values["camera"], values["x11"]), ("0", "0"))

    def test_copy_into_a_new_system(self):
        self.install()
        target = self.root / "target/var/lib/locachy/apps/catalog"
        apps.copy_catalog(str(self.catalog()), str(target))
        self.assertEqual(sorted(apps.read_catalog(str(target))), ["edge", "gimp", "mpv"])
        self.assertEqual(apps.read_meta(str(target))["commit"], self.c1)
        (self.catalog() / "CATALOG").unlink()
        with self.assertRaisesRegex(apps.CatalogError, "no CATALOG"):
            apps.copy_catalog(str(self.catalog()), str(self.root / "other"))

    def test_bootstrap_takes_the_old_presets_but_never_replaces_a_catalog(self):
        legacy = self.root / "share-containers"
        legacy.mkdir()
        for name, text in (("foot.conf", FOOT), ("edge.conf", EDGE), ("gimp.conf", GIMP)):
            (legacy / name).write_text(text)
        commit_file = self.root / "source-commit"
        commit_file.write_text("b" * 40 + "\n")
        said = apps.bootstrap(str(legacy), str(self.apps_dir), str(commit_file))
        self.assertIn("the 2 of the system before are the catalog", said)
        self.assertEqual(sorted(apps.read_catalog(str(self.catalog()))), ["edge", "gimp"])  # foot: the system's
        meta = apps.read_meta(str(self.catalog()))
        self.assertEqual((meta["origin"], meta["commit"], meta["system_commit"]), ("bootstrap", "", "b" * 40))
        before = {p.name: p.read_bytes() for p in self.catalog().iterdir()}
        (legacy / "edge.conf").write_text(EDGE.replace("wayland", "x11"))
        self.assertIn("left as it is", apps.bootstrap(str(legacy), str(self.apps_dir), str(commit_file)))
        self.assertEqual({p.name: p.read_bytes() for p in self.catalog().iterdir()}, before)
        only_host = self.root / "only-host"
        only_host.mkdir()
        (only_host / "foot.conf").write_text(FOOT)
        empty = self.root / "empty-apps"
        self.assertIn("none to keep", apps.bootstrap(str(only_host), str(empty), str(commit_file)))
        self.assertFalse(empty.exists())
        # make.sh: the old presets are read before it replaces them, and only
        # the host programs' go there.
        make = (SOURCE / "make.sh").read_text()
        self.assertLess(make.index('locachy_apps.py" bootstrap /usr/local/share/locachy/containers'),
                        make.index("rm -rf /usr/local/share/locachy/containers"))
        self.assertIn("grep -q '^host=\"1\"' \"$preset\" || install -m 644 \"$preset\" "
                      "/usr/local/share/locachy/containers/", make)
        self.assertEqual(sorted(p.name for p in (SOURCE / "containers").glob("*.conf")), ["code.conf", "foot.conf"])
        tools = (SOURCE / "link/install-pkg-tools").read_text()
        self.assertIn("locachy_apps.py", tools)

    def test_make_keeps_the_old_presets_when_no_catalog_is_made(self):
        """make.sh's preset step, run on private files: a failed bootstrap
        keeps the old presets there for the next run to try again; a done one
        leaves only the host programs' there."""
        make = (SOURCE / "make.sh").read_text()
        start = make.index('if python3 -I "$src/link/locachy_apps.py" bootstrap')
        end = make.index("\ndone\n", start) + len("\ndone\n")
        legacy = self.root / "share-containers"
        src = self.root / "src"
        (src / "containers").mkdir(parents=True)
        (src / "containers/foot.conf").write_text(FOOT)
        (src / "containers/edge.conf").write_text(EDGE)  # not a host program: never put there
        for bootstrap, there in (("false", ["edge.conf", "foot.conf", "gimp.conf"]), ("true", ["foot.conf"])):
            shutil.rmtree(legacy, ignore_errors=True)
            legacy.mkdir()
            for name, text in (("foot.conf", FOOT), ("edge.conf", EDGE), ("gimp.conf", GIMP)):
                (legacy / name).write_text(text)
            step = make[start:end].replace("/usr/local/share/locachy/containers", str(legacy))
            step = step.replace('python3 -I "$src/link/locachy_apps.py"', bootstrap)
            r = subprocess.run(["sh", "-euc", step], env=dict(os.environ, src=str(src)),
                               capture_output=True, text=True, timeout=10)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(sorted(p.name for p in legacy.iterdir()), there)
            self.assertEqual((legacy / "edge.conf").read_text() if bootstrap == "false" else EDGE, EDGE)
            self.assertEqual("they stay in" in r.stderr, bootstrap == "false")


class Updater(Base):
    """locachy-pkg's apps target, its plan rows and their application."""

    def setUp(self):
        super().setUp()
        self.rc = self.root / "rc.conf"
        self.rc.write_text(f'apps_repo="{self.url}"\n')
        self.src = self.root / "src"
        self.src.mkdir()
        (self.root / "built-host").write_text("host-current\n")
        for name, value in (("APPS_DIR", self.apps_dir), ("CONF_DIR", self.conf), ("RC_CONF", self.rc),
                            ("TRUSTED_SIGNERS", self.signers), ("SRC", self.src),
                            ("STATE_DIR", self.root / "state"), ("PLAN_FILE", self.root / "plan"),
                            ("LOCAL_RECORDS", self.root / "records"), ("MANIFEST", self.root / "packages"),
                            ("BUILT_HOST_FILE", self.root / "built-host"),
                            ("SOURCE_UNBUILT_FILE", self.root / "source-unbuilt"),
                            ("SOURCE_COMMIT_FILE", self.root / "source-commit"),
                            ("SOURCE_REPO_DIR", self.root / "repo")):
            self.patch(name, str(value))
        self.patch("in_net", lambda command: command)
        self.patch("on_host_root", lambda: False)
        self.patch("local_packages", lambda db: {})
        self.patch("repo_updates", lambda: ([], {}))
        self.patch("aur_versions", lambda names: self.fail(f"AUR asked for {names}"))
        self.patch("host_hash", lambda: "host-current")
        self.patch("run", lambda command, net=False: self.fail(f"ran {command}"))

    def patch(self, name, value):
        p = mock.patch.object(pkg, name, value)
        p.start()
        self.addCleanup(p.stop)

    def quiet(self):
        stack = contextlib.ExitStack()
        out, err = io.StringIO(), io.StringIO()
        stack.enter_context(contextlib.redirect_stdout(out))
        stack.enter_context(contextlib.redirect_stderr(err))
        return stack, out, err

    def test_plan_rows_notes_and_off(self):
        stack, _, err = self.quiet()
        with stack:
            rows = pkg.plan()
        self.assertEqual(rows, [("apps", "repo", "locachy-apps", "none", f"{self.c1[:7]} (First presets)"),
                                ("apps", "catalog", "app presets from branch main", "none", self.c1)])
        self.assertIn(f"locachy-pkg: app presets from {self.url} (main)", err.getvalue())
        self.assertEqual(pkg.targets_of(rows), ["apps"])
        for off in ("off", "0", ""):
            self.rc.write_text(f'apps_repo="{off}"\n')
            self.assertEqual(pkg.apps_rows(), [])
        self.rc.write_text(f'apps_repo="{self.url}"\napps_branch="a..b"\n')
        with self.quiet()[0]:
            self.assertEqual(pkg.apps_rows(), [("apps", "note", f"not checked: apps_branch 'a..b' in {self.rc} "
                                                "is not a branch name", "", "")])

    def test_fetch_failure_keeps_the_old_catalog_with_a_note(self):
        self.install()
        before = {p.name: p.read_bytes() for p in self.catalog().iterdir()}
        gone = self.root / "gone"
        self.rc.write_text(f'apps_repo="file://{gone}"\n')
        stack, out, err = self.quiet()
        with stack:
            rows = pkg.plan()
            pkg.print_plan(rows, "2026-10-30 10:00")
        self.assertEqual(len(rows), 1, rows)
        target, kind, what, old, new = rows[0]
        self.assertEqual((target, kind, old, new), ("apps", "note", "", ""))
        self.assertTrue(what.startswith(f"not checked: could not fetch file://{gone} (main): "), what)
        self.assertIn("locachy-pkg: app presets not checked: could not fetch", err.getvalue())
        self.assertIn("Note: apps " + what + "\n", out.getvalue())
        self.assertEqual(pkg.targets_of(rows), [])
        self.assertEqual({p.name: p.read_bytes() for p in self.catalog().iterdir()}, before)
        # Invalid or unsigned content: a note too, the catalog as it was.
        self.rc.write_text(f'apps_repo="{self.url}"\n')
        self.commit({"apps/edge.conf": EDGE + 'host="1"\n'}, "Edge a host program")
        with self.quiet()[0]:
            self.assertEqual(pkg.apps_rows(), [("apps", "note", "not checked: edge.conf: line 9: host programs are "
                                                "the system's, never an app preset", "", "")])
        self.assertEqual({p.name: p.read_bytes() for p in self.catalog().iterdir()}, before)

    def test_bootstrap_catalog_is_noted_until_one_from_the_repository(self):
        legacy = self.root / "legacy"
        legacy.mkdir()
        (legacy / "edge.conf").write_text(EDGE)
        apps.bootstrap(str(legacy), str(self.apps_dir), str(self.root / "source-commit"))
        note = ("apps", "note", "bootstrap catalog from the system source, not from the apps repository", "", "")
        with self.quiet()[0]:
            rows = pkg.plan()
        self.assertIn(("apps", "catalog", "app presets from branch main", "bootstrap", self.c1), rows)
        self.assertEqual(rows[-1], note)
        self.rc.write_text('apps_repo="off"\n')
        self.assertEqual(pkg.plan(), [])  # off: nothing, not even the note
        self.rc.write_text(f'apps_repo="file://{self.root / "gone"}"\n')
        with self.quiet()[0]:
            rows = pkg.plan()
        self.assertEqual([r[2][:12] for r in rows], ["not checked:", note[2][:12]])

    def test_a_merge_that_changes_packages_rebuilds_the_container_in_the_same_apply(self):
        self.install()
        (self.conf / "gimp.conf").write_text(GIMP)
        self.commit({"apps/gimp.conf": GIMP.replace('extras=""', 'extras="gimp-help-en"')}, "GIMP's help")
        with self.quiet()[0]:
            rows = pkg.plan()
        self.assertIn(("apps", "merge", "gimp", "extras", ""), rows)
        self.assertIn(("gimp", "rebuild", "its preset names other packages", "gimp", "gimp gimp-help-en"), rows)
        self.assertEqual(pkg.targets_of(rows), ["apps", "gimp"])
        stack, out, _ = self.quiet()
        with stack:
            pkg.print_plan(rows, "2026-10-30 10:00")
        self.assertIn("  merge gimp: extras as its new preset has them\n", out.getvalue())
        self.assertIn(f"  app presets from branch main ({self.c1[:7]} -> ", out.getvalue())
        built = []
        self.patch("run", lambda command, net=False: built.append(command) or True)
        pkg.save_plan(rows)
        with self.quiet()[0]:
            self.assertEqual(pkg.cmd_apply([]), 0)
        self.assertIn('extras="gimp-help-en"', (self.conf / "gimp.conf").read_text())
        self.assertEqual(built, [["/usr/local/bin/locachy-app", "build", "gimp"]])
        # A merge that keeps the packages rebuilds nothing.
        self.commit({"apps/gimp.conf": GIMP.replace('extras=""', 'extras="gimp-help-en"').replace(
            'network="0"', 'network="1"')}, "GIMP on the network")
        with self.quiet()[0]:
            rows = pkg.apps_rows()
        self.assertIn(("apps", "merge", "gimp", "network", ""), rows)
        self.assertFalse(any(r[1:3] == ("rebuild", pkg.PRESET_REBUILD) for r in rows))

    def test_apps_only_apply_builds_nothing_and_applies_the_checked_commit(self):
        self.install()
        (self.conf / "edge.conf").write_text(EDGE)
        (self.root / "state/edge").mkdir(parents=True)  # built, against this host: nothing else to do
        (self.root / "state/edge/built").write_text('packages="microsoft-edge-stable-bin"\nhost="host-current"\n')
        c2 = self.commit({"apps/edge.conf": EDGE.replace('camera="1"', 'camera="0"')}, "Edge without cameras")
        stack, out, _ = self.quiet()
        with stack:
            self.assertEqual(pkg.cmd_plan([]), 0)
        self.assertEqual(pkg.planned_apps_commit(pkg.load_plan()[0]), c2)
        c3 = self.commit({"apps/edge.conf": EDGE.replace('camera="1"', 'camera="0"').replace("Edge", "Edge 3")},
                         "Newer")
        commands = []
        real = subprocess.Popen

        def popen(args, *a, **kw):
            commands.append(list(args) if isinstance(args, (list, tuple)) else [args])
            return real(args, *a, **kw)
        stack, out, _ = self.quiet()
        with stack, mock.patch.object(subprocess, "Popen", popen), \
                mock.patch.object(pkg, "write_source_unbuilt", lambda *a, **kw: self.fail("unbuilt marker")), \
                mock.patch.object(pkg, "rebuild_locachy", lambda force=False: self.fail("make.sh")):
            self.assertEqual(pkg.cmd_apply([]), 0)
        said = out.getvalue()
        self.assertIn("===> apps\n===> apps: catalog " + c2[:7] + " installed\n"
                      "===> apps: edge: camera as its new preset has them\n===> apps: done\n", said)
        self.assertIn("===> all done", said)
        for command in commands:
            for word in command:
                self.assertFalse(str(word).endswith(BUILDS), command)
        self.assertFalse(Path(pkg.SOURCE_UNBUILT_FILE).exists())
        self.assertEqual(apps.read_meta(str(self.catalog()))["commit"], c2)  # the checked one
        self.assertIn('camera="0"', (self.conf / "edge.conf").read_text())
        rows, _ = pkg.load_plan()  # the Check after it offers the newer one
        self.assertEqual(pkg.planned_apps_commit(rows), c3)

    def test_a_failed_apps_update_stops_nothing_else(self):
        self.install()
        (self.conf / "gimp.conf").write_text(GIMP)
        rows = [("apps", "catalog", "app presets from branch main", self.c1[:7], "e" * 40),
                ("apps", "merge", "gimp", "extras", ""),
                ("gimp", "rebuild", pkg.PRESET_REBUILD, "gimp", "gimp gimp-help-en"),
                ("host", "repo", "base", "1", "2"), ("locachy", "rebuild", "its programs", "", "")]
        pkg.save_plan(rows)
        applied = []
        real = pkg.apply_target
        self.patch("apply_target", lambda t, r: real(t, r) if t == "apps" else applied.append(t) or True)
        self.patch("plan", lambda **kw: self.fail("a failed plan is kept"))
        stack, out, err = self.quiet()
        with stack:
            self.assertEqual(pkg.cmd_apply([]), 1)
        self.assertEqual(applied, ["host", "locachy"])  # not gimp: its merge was not done
        self.assertIn("===> apps: not done", out.getvalue())
        self.assertIn("===> not done: apps", out.getvalue())
        self.assertIn("app presets not updated, the installed catalog stays", err.getvalue())
        self.assertEqual(apps.read_meta(str(self.catalog()))["commit"], self.c1)

    def test_a_replan_after_the_source_uses_the_pinned_commit_without_fetching(self):
        self.install()
        c2 = self.commit({"apps/gimp.conf": GIMP.replace('network="0"', 'network="1"')}, "GIMP on the network")
        with self.quiet()[0]:
            pkg.save_plan(pkg.plan())
        self.commit({"apps/gimp.conf": GIMP.replace("GIMP", "GIMP 3")}, "Newer")
        self.rc.write_text(f'apps_repo="file://{self.root / "gone"}"\n')  # a fetch would fail: a note
        rows = pkg.plan(check_source=False, apps_commit=pkg.planned_apps_commit(pkg.load_plan()[0]))
        self.assertIn(("apps", "catalog", "app presets from branch main", self.c1[:7], c2), rows)
        self.assertFalse(any(r[1] == "note" for r in rows))
        # Not in repo.git (the earlier plan was the old updater's, or none):
        # the apps part waits for the next Check.
        self.assertEqual(pkg.plan(check_source=False, apps_commit="f" * 40), [])
        self.assertEqual(pkg.plan(check_source=False), [])
        # cmd_apply's second phase passes the pinned commit on.
        seen = []
        self.patch("plan", lambda check_source=True, apps_commit=None: seen.append((check_source, apps_commit)) or [])
        with self.quiet()[0], self.assertRaises(SystemExit):
            pkg.cmd_apply([], after_source=True)  # that plan is empty: nothing to apply
        self.assertEqual(seen, [(False, c2)])

    def test_search_reads_the_catalog_and_skips_host_programs(self):
        catalog = self.root / "search-catalog"
        catalog.mkdir()
        (catalog / "edge.conf").write_text(EDGE)
        (catalog / "foot.conf").write_text(FOOT)
        (catalog / "CATALOG").write_text('format="1"\norigin="medium"\n')
        nothing = lambda *a, **kw: subprocess.CompletedProcess(a, 1, "", "")
        stack, out, _ = self.quiet()
        with stack, mock.patch.dict(os.environ, {"LOCACHY_APPS_CATALOG": str(catalog)}), \
                mock.patch.object(pkg.subprocess, "run", nothing):
            self.assertEqual(pkg.cmd_search(["edge"]), 0)
            self.assertEqual(pkg.cmd_search(["foot"]), 0)
        self.assertEqual(out.getvalue().splitlines(), [
            "edge\trecipe\tmicrosoft-edge-stable-bin\t\t/usr/bin/microsoft-edge-stable\t--ozone-platform=wayland\t1\t"
            "LOCachy preset: Microsoft Edge, on the network."])
        self.assertNotIn("LOCACHY_PRESETS", (SOURCE / "link/locachy-pkg").read_text())


if __name__ == "__main__":
    unittest.main()
