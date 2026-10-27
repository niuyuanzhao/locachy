#!/usr/bin/env python3
"""Stable upstream candidates without network, pacman, root or makepkg.
The adapter is separately sourced by bash with synthetic upstream functions;
that fixture never compiles packages or touches the live host.
  python3 -B tools/test-upstream.py SCRATCH_PARENT
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.parse

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("upstream", SOURCE / "link/locachy_upstream.py")
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class StableUpstream(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="upstream-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.baseline = self.root / "baseline"
        shutil.copytree(SOURCE / "pkgbuilds/labwc-locachy", self.baseline)
        self.state = self.root / "state"
        self.commit = "a" * 40
        self.version = "0.21.0"
        self.release = "2"
        self.urls = []
        self.remote = {"PKGBUILD": b"touch /THIS-MUST-NOT-RUN-ON-HOST\npkgname=labwc\n",
                       ".SRCINFO": self.info(), "fix.patch": b"upstream patch\n",
                       "LICENSES/GPL.txt": b"license\n"}

    def info(self):
        return ("pkgbase = labwc\n\tpkgver = " + self.version + "\n\tpkgrel = " + self.release +
                "\n\tpkgdesc = Stable compositor\n\tarch = x86_64\n\tdepends = new-wlroots\n"
                "\toptions = !lto\n\tsource = https://example.invalid/labwc.tar.gz\n\tsha256sums = abc\n"
                "pkgname = labwc\n").encode()

    def fetch(self, url, **kwargs):
        self.urls.append(url)
        self.assertLessEqual(kwargs["max_bytes"], u.MAX_FILE_BYTES)
        self.assertEqual(kwargs["timeout"], u.NETWORK_TIMEOUT)
        if "/repository/tags/" in url:
            tag = urllib.parse.unquote(url.rsplit("/", 1)[1])
            return json.dumps({"name": tag, "commit": {"id": self.commit}}).encode()
        if "/repository/tree?" in url:
            self.assertIn("ref=" + self.commit, url)
            entries = [{"path": "LICENSES", "mode": "040000", "type": "tree"}]
            entries += [{"path": n, "mode": "100644", "type": "blob"} for n in self.remote]
            return json.dumps(entries).encode()
        encoded = url.split("/repository/files/", 1)[1].split("/raw?", 1)[0]
        return self.remote[urllib.parse.unquote(encoded)]

    def candidate(self, repo="extra", version=None, **kwargs):
        return u.prepare_candidate("labwc-locachy", self.baseline, repo,
                                   version or self.version + "-" + self.release,
                                   self.state, stable_version=kwargs.pop("stable_version", self.version + "-" + self.release),
                                   fetch=kwargs.pop("fetch", self.fetch), **kwargs)

    def test_candidate_is_immutable_and_host_executes_nothing(self):
        def forbidden(*args, **kwargs):
            self.fail("check executed a process")
        lock_path = self.candidate(run=forbidden)
        lock = u.validate_candidate(lock_path, "labwc-locachy", self.baseline, self.state)
        self.assertEqual(lock["packaging_commit"], self.commit)
        self.assertEqual(lock["package_version"], "1:0.21.0-2")
        self.assertEqual(lock["source_version"], "0.21.0-2")
        self.assertEqual((lock_path.parent / "upstream/PKGBUILD").read_bytes(), self.remote["PKGBUILD"])
        self.assertEqual((lock_path.parent / "recipe/fix.patch").read_bytes(), self.remote["fix.patch"])
        self.assertEqual(self.candidate(), lock_path)
        self.assertNotEqual(lock["effective_recipe_hash"], lock["local_input_hash"])
        self.assertIn("pkgver='0.21.0'", (lock_path.parent / "recipe/PKGBUILD").read_text())

    def test_source_is_independent_of_target_repository_and_rebuild_version(self):
        recipe_hash = None
        for repo, version in (("extra", "0.21.0-2"), ("extra-x86-64-v2", "0.21.0-2.1"),
                              ("cachyos-extra-v3", "0.21.0-2.7"), ("cachyos-extra-v4", "0.21.0-2.9"),
                              ("future-optimized", "9:0.99-17.23")):
            lock = json.loads(self.candidate(repo, version).read_bytes())
            self.assertEqual(lock["repository"], repo)
            self.assertEqual(lock["repository_version"], version)
            self.assertEqual(lock["source_repository"], "extra")
            self.assertEqual(lock["source_version"], "0.21.0-2")
            self.assertEqual(lock["upstream_tag"], "0.21.0-2")
            self.assertEqual(lock["package_version"], "1:0.21.0-2")
            self.assertEqual(lock["packaging_commit"], self.commit)
            recipe_hash = recipe_hash or lock["effective_recipe_hash"]
            self.assertEqual(lock["effective_recipe_hash"], recipe_hash)
        self.assertTrue(all("/repository/tags/0.21.0-2" in url for url in self.urls if "/repository/tags/" in url))

    def test_stable_source_is_required_and_invalid_versions_never_fetch(self):
        with self.assertRaises(TypeError):
            u.prepare_candidate("labwc-locachy", self.baseline, "cachyos-extra-v3", "0.21.0-2.1", self.state)
        for version in ("", "latest", "0.21.0", "../../other"):
            with self.assertRaises(u.UpstreamError):
                self.candidate(stable_version=version)
        self.assertEqual(self.urls, [])

    def test_source_metadata_must_match_official_extra(self):
        self.remote[".SRCINFO"] = self.info().replace(b"pkgrel = 2", b"pkgrel = 3")
        with self.assertRaisesRegex(u.UpstreamError, "version does not match"):
            self.candidate()

    def test_split_package_requires_review(self):
        self.remote[".SRCINFO"] += b"pkgname = labwc-tools\n"
        with self.assertRaisesRegex(u.UpstreamError, "split"):
            self.candidate()

    def test_pkgrel_only_update_gets_another_candidate(self):
        a = self.candidate()
        self.release = "3"
        self.remote[".SRCINFO"] = self.info()
        b = self.candidate()
        self.assertNotEqual(a, b)
        self.assertEqual(json.loads(b.read_bytes())["package_version"], "1:0.21.0-3")

    def test_moved_tag_commit_has_distinct_identity(self):
        a = self.candidate()
        self.commit = "b" * 40
        b = self.candidate()
        self.assertNotEqual(a, b)

    def test_local_patch_change_produces_new_candidate_without_version_mapping(self):
        first = self.candidate("cachyos-extra-v3", "0.21.0-2.1")
        with open(self.baseline / "clipboard-guard.patch", "a") as f:
            f.write("new local patch\n")
        second = self.candidate("cachyos-extra-v3", "0.21.0-2.1")
        self.assertNotEqual(first, second)
        a, b = (json.loads(p.read_text()) for p in (first, second))
        self.assertEqual(a["source_version"], b["source_version"])
        self.assertEqual(a["packaging_commit"], b["packaging_commit"])
        self.assertNotEqual(a["effective_recipe_hash"], b["effective_recipe_hash"])
        self.assertFalse((self.state / "upstream/labwc-locachy/active.json").exists())

    def test_offline_does_not_promote_or_replace_a_candidate(self):
        a = self.candidate()
        def offline(*args, **kwargs):
            raise u.UpstreamError("offline")
        with self.assertRaises(u.UpstreamError):
            self.candidate(fetch=offline)
        self.assertEqual(u.effective_recipe("labwc-locachy", self.baseline, self.state), self.baseline)
        self.assertTrue(a.is_file())

    def test_symlinks_and_path_escape_are_rejected(self):
        original = self.fetch
        def malicious(url, **kwargs):
            if "/repository/tree?" in url:
                return json.dumps([{"path": "PKGBUILD", "mode": "120000", "type": "blob"}]).encode()
            if "/repository/files/PKGBUILD/raw?" in url:
                return b"../../outside"
            return original(url, **kwargs)
        with self.assertRaisesRegex(u.UpstreamError, "unsafe"):
            self.candidate(fetch=malicious)
        def traversal(url, **kwargs):
            if "/repository/tree?" in url:
                return json.dumps([{"path": "../escape", "mode": "100644", "type": "blob"}]).encode()
            return original(url, **kwargs)
        with self.assertRaisesRegex(u.UpstreamError, "unsafe"):
            self.candidate(fetch=traversal)

    def test_local_change_invalidates_lock_and_active_recipe(self):
        lock = self.candidate()
        self.promote(lock)
        with open(self.baseline / "clipboard-guard.patch", "a") as f:
            f.write("changed\n")
        with self.assertRaisesRegex(u.UpstreamError, "changed since Check"):
            u.validate_candidate(lock, "labwc-locachy", self.baseline, self.state)
        with self.assertRaises(u.UpstreamError):
            u.effective_recipe("labwc-locachy", self.baseline, self.state)

    def test_modified_upstream_or_effective_tree_fails_validation(self):
        lock = self.candidate()
        f = lock.parent / "recipe/fix.patch"
        f.write_text("tampered")
        with self.assertRaisesRegex(u.UpstreamError, "effective"):
            u.validate_candidate(lock, "labwc-locachy", self.baseline, self.state)

    def promote(self, lock):
        artifact = "b" * 64
        return u.promote_candidate(lock, "labwc-locachy", self.baseline, self.state,
            artifact_sha256=artifact, gate={"id": "labwc-clipboard-freeze-v1", "passed": True,
                                           "artifact_sha256": artifact, "test_hash": "c" * 64})

    def test_promotion_requires_passed_matching_artifact_gate(self):
        lock = self.candidate()
        with self.assertRaises(u.UpstreamError):
            u.promote_candidate(lock, "labwc-locachy", self.baseline, self.state,
                                artifact_sha256="b" * 64, gate={"passed": True})
        self.assertEqual(u.effective_recipe("labwc-locachy", self.baseline, self.state), self.baseline)
        recipe = self.promote(lock)
        self.assertEqual(u.effective_recipe("labwc-locachy", self.baseline, self.state), recipe)

    def test_candidate_lock_tampering_fails(self):
        lock = self.candidate()
        data = json.loads(lock.read_bytes())
        data["packaging_commit"] = "c" * 40
        lock.write_text(json.dumps(data))
        with self.assertRaisesRegex(u.UpstreamError, "changed"):
            u.validate_candidate(lock, "labwc-locachy", self.baseline, self.state)

    def test_adapter_preserves_upstream_metadata_and_functions(self):
        self.remote["PKGBUILD"] = b"""pkgname=labwc
pkgver=0.21.0
pkgrel=2
pkgdesc='upstream description'
arch=(x86_64)
depends=(new-wlroots cairo)
makedepends=(meson)
options=(!lto debug)
source=('release.tar.gz')
sha256sums=('upstream-sum')
prepare() { printf 'prepare:%s:%s\\n' \"$pkgname\" \"${options[*]}\" >>\"$TEST_LOG\"; }
build() { printf 'build:%s:%s\\n' \"$pkgname\" \"${depends[*]}\" >>\"$TEST_LOG\"; }
check() { printf 'check:%s\\n' \"$pkgname\" >>\"$TEST_LOG\"; }
package() { printf 'package:%s\\n' \"$pkgname\" >>\"$TEST_LOG\"; }
"""
        lock = self.candidate()
        recipe = lock.parent / "recipe"
        (recipe / "src/labwc/src").mkdir(parents=True)
        (recipe / "src/labwc/include").mkdir()
        for name in ("clipboard-guard.patch", "locachy-clipboard.c", "locachy-clipboard.h"):
            shutil.copyfile(recipe / name, recipe / "src" / name)
        binary = self.root / "bin"
        binary.mkdir()
        patch = binary / "patch"
        patch.write_text('#!/bin/sh\nprintf "patch:%s\\n" "$*" >>"$TEST_LOG"\n')
        patch.chmod(0o755)
        script = r'''set -euo pipefail
startdir=$PWD
srcdir=$PWD/src
source ./PKGBUILD
[[ $pkgname == labwc-locachy && $epoch == 1 && $pkgver == 0.21.0 && $pkgrel == 2 ]]
[[ ${depends[*]} == 'new-wlroots cairo' && ${options[*]} == '!lto debug' ]]
[[ ${#source[@]} == 4 && ${#sha256sums[@]} == 4 && ${sha256sums[0]} == upstream-sum ]]
prepare
build
check
package
[[ $pkgname == labwc-locachy ]]
'''
        log = self.root / "calls"
        env = dict(os.environ, TEST_LOG=str(log), PATH=str(binary) + os.pathsep + os.environ["PATH"])
        p = subprocess.run(["bash", "-c", script], cwd=recipe, env=env, text=True, capture_output=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = log.read_text()
        self.assertIn("prepare:labwc:!lto debug", lines)
        self.assertIn("build:labwc:new-wlroots cairo", lines)
        self.assertIn("check:labwc", lines)
        self.assertIn("package:labwc", lines)
        self.assertIn("--batch --forward --fuzz=0", lines)
        self.assertEqual((recipe / "src/labwc/src/locachy-clipboard.c").read_bytes(),
                         (self.baseline / "locachy-clipboard.c").read_bytes())


class BuildFingerprint(unittest.TestCase):
    """A local package is rebuilt for what it builds and runs against only."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="fingerprint-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.db = self.root / "local"
        self.recipe = self.root / "recipe"
        self.recipe.mkdir()
        (self.recipe / "PKGBUILD").write_text(
            "pkgname=thing\ndepends=(cairo 'libwl.so>=1'  # the compositor's\n  pango)\n"
            "makedepends=(meson \"$_extra\")\noptdepends=('never: not built against')\n")
        for name, version, deps, provides in (
                ("base-devel", "1-1", ["gcc"], []), ("gcc", "15-1", ["glibc"], []), ("glibc", "2.42-1", [], []),
                ("cairo", "1.18-1", ["glibc", "pixman>=0.40"], []), ("pixman", "0.46-1", [], []),
                ("pango", "1.56-1", ["cairo"], []), ("wlroots", "0.20-1", [], ["libwl.so=1-64"]),
                ("meson", "1.9-1", ["python"], []), ("python", "3.14-1", [], []),
                ("never", "1-1", [], []), ("thing", "1-1", ["cairo"], [])):
            self.package(name, version, deps, provides)

    def package(self, name, version, deps=(), provides=()):
        directory = self.db / f"{name}-{version}"
        for old in self.db.glob(f"{name}-*"):
            shutil.rmtree(old)
        directory.mkdir(parents=True)
        text = f"%NAME%\n{name}\n\n%VERSION%\n{version}\n\n"
        if deps:
            text += "%DEPENDS%\n" + "\n".join(deps) + "\n\n"
        if provides:
            text += "%PROVIDES%\n" + "\n".join(provides) + "\n\n"
        (directory / "desc").write_text(text)

    def fingerprint(self):
        return u.build_fingerprint(self.recipe, {"thing"}, self.db)

    def test_dependencies_are_read_not_run(self):
        self.assertEqual(u.recipe_dependencies(self.recipe), {"base-devel", "cairo", "libwl.so", "pango", "meson"})
        (self.recipe / ".locachy-upstream.PKGBUILD").write_text("checkdepends=(\n  python\n)\n")
        self.assertIn("python", u.recipe_dependencies(self.recipe))

    def test_only_its_closure_counts(self):
        before = self.fingerprint()
        expected = "".join(f"{n} {v}\n" for n, v in sorted({
            "base-devel": "1-1", "gcc": "15-1", "glibc": "2.42-1", "cairo": "1.18-1", "pixman": "0.46-1",
            "pango": "1.56-1", "wlroots": "0.20-1", "meson": "1.9-1", "python": "3.14-1"}.items()))
        self.assertEqual(before, hashlib.sha256(expected.encode()).hexdigest())
        self.package("fastfetch", "2.70-1")
        self.package("adwaita-cursors", "51-1")
        self.package("never", "2-1")
        self.package("thing", "2-1", ["cairo"])
        self.assertEqual(self.fingerprint(), before)  # unrelated, optional, or itself: no rebuild
        self.package("pixman", "0.47-1")
        self.assertNotEqual(self.fingerprint(), before)  # a library it builds against, through cairo
        middle = self.fingerprint()
        self.package("gcc", "15.1-1", ["glibc"])
        self.assertNotEqual(self.fingerprint(), middle)  # the toolchain (base-devel)


if __name__ == "__main__":
    unittest.main()
