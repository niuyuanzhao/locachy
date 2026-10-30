#!/usr/bin/env python3
"""Tracked libpaper overlay metadata and adapter; no network or root."""
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

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("paper_upstream", SOURCE / "link/locachy_upstream.py")
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class PaperOverlay(unittest.TestCase):
    def test_locked_candidate_keeps_official_recipe_and_applies_only_the_patch(self):
        with tempfile.TemporaryDirectory(prefix="paper-overlay-", dir=SCRATCH) as tmp:
            root = Path(tmp)
            baseline = root / "baseline"
            shutil.copytree(SOURCE / "pkgbuilds/libpaper", baseline)
            commit = "b" * 40
            remote = {
                "PKGBUILD": b'''pkgname=libpaper
pkgver=2.2.8
pkgrel=1
source=(https://example.invalid/libpaper.tar.gz)
sha256sums=(official)
depends=(glibc)
prepare() { printf 'prepare:%s\\n' "$pkgname" >>"$TEST_LOG"; }
build() { printf 'build:%s\\n' "$pkgname" >>"$TEST_LOG"; }
check() { printf 'check:%s\\n' "$pkgname" >>"$TEST_LOG"; }
package() { printf 'package:%s\\n' "$pkgname" >>"$TEST_LOG"; }
''',
                ".SRCINFO": b'''pkgbase = libpaper
pkgver = 2.2.8
pkgrel = 1
pkgdesc = Paper sizes
arch = x86_64
source = https://example.invalid/libpaper.tar.gz
sha256sums = official
pkgname = libpaper
''',
            }
            def fetch(url, **limits):
                if "/repository/tags/" in url:
                    return json.dumps({"name": "2.2.8-1", "commit": {"id": commit}}).encode()
                if "/repository/tree?" in url:
                    return json.dumps([{"path": n, "mode": "100644", "type": "blob"} for n in remote]).encode()
                name = urllib.parse.unquote(url.split("/repository/files/", 1)[1].split("/raw?", 1)[0])
                return remote[name]
            state = root / "state"
            lock_path = u.prepare_candidate("libpaper", baseline, "extra", "2.2.8-1", state,
                                             stable_version="2.2.8-1", fetch=fetch)
            lock = u.validate_candidate(lock_path, "libpaper", baseline, state)
            self.assertEqual(lock["gate"], "libpaper-lifecycle-v1")
            self.assertEqual(lock["package_version"], "1:2.2.8-1")
            recipe = lock_path.parent / "recipe"
            self.assertEqual((recipe / ".locachy-upstream.PKGBUILD").read_bytes(), remote["PKGBUILD"])
            (recipe / "src/libpaper-2.2.8").mkdir(parents=True)
            shutil.copyfile(recipe / "lifecycle.patch", recipe / "src/lifecycle.patch")
            binary = root / "bin"
            binary.mkdir()
            patch = binary / "patch"
            patch.write_text('#!/bin/sh\nprintf "patch:%s\\n" "$*" >>"$TEST_LOG"\n')
            patch.chmod(0o755)
            log = root / "calls"
            env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"], TEST_LOG=str(log))
            script = '''set -e
startdir=$PWD
srcdir=$PWD/src
source ./PKGBUILD
[[ $pkgname == libpaper && $epoch == 1 && ${depends[*]} == glibc ]]
[[ ${#source[@]} == 2 && ${#sha256sums[@]} == 2 && ${sha256sums[0]} == official ]]
prepare; build; check; package
'''
            result = subprocess.run(["bash", "-c", script], cwd=recipe, env=env,
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = log.read_text()
            self.assertIn("prepare:libpaper", calls)
            self.assertIn("patch:--batch --forward --fuzz=0", calls)
            self.assertIn("lifecycle.patch", calls)
            self.assertIn("check:libpaper", calls)
            self.assertIn("package:libpaper", calls)


if __name__ == "__main__":
    unittest.main()
