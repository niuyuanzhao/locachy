#!/usr/bin/env python3
"""The CLI-only split overlay, without network, makepkg, mounts or pacman.
Only a synthetic upstream recipe is sourced by bash, never the live host's.
  python3 -B tools/test-libinput-tools.py SCRATCH_PARENT
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

# Synthetic Arch recipe retaining the reviewed patch anchors. No downloaded
# package code is run by this test; staging and compilation are stand-ins.
RECIPE = '''pkgbase=libinput
pkgname=(
  libinput
  libinput-tools
)
pkgver=1.32.0
pkgrel=1
pkgdesc="Input library"
arch=(x86_64)
depends=(
  glibc
  libevdev
  libgcc
  libwacom
  lua54
  mtdev
  systemd-libs
)
makedepends=(
  cairo
  check
  git
  glib2
  gtk4
  libx11
  meson
  python
  python-libevdev
  python-pyudev
  python-yaml
  wayland
  wayland-protocols
)
checkdepends=(python-pytest)
source=(
  'release.tar.gz'
)
sha256sums=('upstream-checksum')
validpgpkeys=('upstream-key')

build() {
  local meson_options=(
    # upstream recommends not building docs
    -D documentation=false
  )

  arch-meson libinput build "${meson_options[@]}"
  meson compile -C build
}

check() {
  printf 'check:%s\\n' "$pkgname" >>"$TEST_LOG"
}
package_libinput() {
  mkdir -p tools/usr/bin tools/usr/lib/libinput "$pkgdir/usr/lib"
  printf library >"$pkgdir/usr/lib/libinput.so.10"
  printf cli >tools/usr/bin/libinput
  printf events >tools/usr/lib/libinput/libinput-debug-events
  printf devices >tools/usr/lib/libinput/libinput-list-devices
}

package_libinput-tools() {
  pkgdesc+=" (debug utilities)"
  depends=(
    cairo
    glib2
    glibc
    gtk4
    libevdev
    libgcc
    libinput
    libx11
    python
    python-libevdev
    python-pyudev
    python-yaml
    systemd-libs
    wayland
  )

  mv tools/* "$pkgdir"

  install -Dm644 libinput/COPYING -t "$pkgdir/usr/share/licenses/$pkgname"
}
'''


class LibinputTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="libinput-tools-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.baseline = self.root / "baseline"
        shutil.copytree(SOURCE / "pkgbuilds/libinput-tools", self.baseline)
        self.state = self.root / "state"
        self.remote = {"PKGBUILD": RECIPE.encode(), ".SRCINFO": self.info()}
        self.urls = []

    def info(self):
        return b'''pkgbase = libinput
\tpkgver = 1.32.0
\tpkgrel = 1
\tpkgdesc = Input library
\tarch = x86_64
\tdepends = libwacom
pkgname = libinput
\tpkgdesc = Library output
\tprovides = libinput.so
pkgname = libinput-tools
\tpkgdesc = CLI and GUI tools
\tdepends = gtk4
\tdepends = libinput
'''

    def fetch(self, url, **kwargs):
        self.urls.append(url)
        self.assertIn("archlinux%2Fpackaging%2Fpackages%2Flibinput", url)
        if "/repository/tags/" in url:
            return json.dumps({"name": "1.32.0-1", "commit": {"id": "a" * 40}}).encode()
        if "/repository/tree?" in url:
            return json.dumps([{"path": n, "type": "blob", "mode": "100644"} for n in self.remote]).encode()
        encoded = url.split("/repository/files/", 1)[1].split("/raw?", 1)[0]
        return self.remote[urllib.parse.unquote(encoded)]

    def candidate(self):
        return u.prepare_candidate("libinput-tools", self.baseline, "extra", "1.32.0-1",
                                   self.state, stable_version="1.32.0-1", fetch=self.fetch)

    def source(self, recipe, script):
        # makepkg exports this metadata helper when it sources a PKGBUILD.
        helper = '''extract_function_variable() {
    local decl
    decl=$(declare -f "$1" | grep -E "^[[:space:]]+$2=\\(") || return
    eval "${decl/#$2/$4}"
}
'''
        return subprocess.run(["bash", "-euc", helper + script], cwd=recipe,
                              env=dict(os.environ, TEST_LOG=str(self.root / "calls")),
                              text=True, capture_output=True)

    def test_candidate_selects_tools_metadata_without_executing_recipe(self):
        self.remote["PKGBUILD"] = b"touch /HOST-MUST-NOT-EXECUTE\n" + self.remote["PKGBUILD"]
        lock_path = self.candidate()
        lock = u.validate_candidate(lock_path, "libinput-tools", self.baseline, self.state)
        self.assertEqual(lock["upstream_pkgbase"], "libinput")
        self.assertEqual(lock["upstream_package"], "libinput-tools")
        self.assertEqual(lock["package_version"], "1:1.32.0-1")
        self.assertEqual(lock["gate"], "libinput-tools-cli-v1")
        wrapper = (lock_path.parent / "recipe/PKGBUILD").read_text()
        self.assertIn("pkgname='libinput-tools'", wrapper)
        self.assertIn("pkgdesc='CLI and GUI tools'", wrapper)
        selected = u._selected_srcinfo(self.remote[".SRCINFO"], "libinput-tools")
        self.assertEqual(selected["depends"], ["gtk4", "libinput"])
        self.assertNotIn("provides", selected)
        self.assertEqual(self.candidate(), lock_path)

    def test_optimized_target_does_not_change_tools_source_or_library_constraint(self):
        lock_path = u.prepare_candidate("libinput-tools", self.baseline, "cachyos-extra-v4", "1.32.0-1.9",
                                        self.state, stable_version="1.32.0-1", fetch=self.fetch)
        lock = u.validate_candidate(lock_path, "libinput-tools", self.baseline, self.state)
        self.assertEqual(lock["repository_version"], "1.32.0-1.9")
        self.assertEqual(lock["source_repository"], "extra")
        self.assertEqual(lock["source_version"], "1.32.0-1")
        result = self.source(lock_path.parent / "recipe", '''
startdir=$PWD
source ./PKGBUILD
[[ ${depends[*]} == *' libinput '* && $epoch == 1 && $pkgrel == 1 ]]
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_split_layout_is_rejected(self):
        for text in (self.info().replace(b"pkgname = libinput-tools", b"pkgname = renamed"),
                     self.info() + b"pkgname = extra-tools\n"):
            self.remote[".SRCINFO"] = text
            with self.assertRaisesRegex(u.UpstreamError, "split"):
                self.candidate()

    def test_config_cannot_redirect_source_authority_or_output(self):
        config = self.baseline / "upstream.json"
        original = json.loads(config.read_text())
        for key, value in (("repository", "https://example.invalid/libinput.git"),
                           ("pkgbase", "other"), ("package", "libinput"),
                           ("gate", "labwc-clipboard-freeze-v1")):
            config.write_text(json.dumps(dict(original, **{key: value})))
            with self.assertRaises(u.UpstreamError):
                self.candidate()
        self.assertEqual(self.urls, [])

    def test_adapter_preserves_build_tests_checksums_and_only_emits_cli_tools(self):
        path = self.candidate().parent / "recipe"
        (path / "src/libinput").mkdir(parents=True)
        (path / "src/libinput/COPYING").write_text("license")
        (path / "pkg/libinput-tools").mkdir(parents=True)
        result = self.source(path, '''
startdir=$PWD
srcdir=$PWD/src
pkgdir=$PWD/pkg/libinput-tools
# makepkg defines package-related helpers before sourcing the recipe.
package_info() { :; }
source ./PKGBUILD
[[ ${pkgname[*]} == libinput-tools && $pkgbase == libinput-tools ]]
[[ $epoch == 1 && $pkgver == 1.32.0 && $pkgrel == 1 ]]
[[ ${depends[*]} == 'glibc libevdev libgcc libinput python python-libevdev python-pyudev python-yaml systemd-libs' ]]
[[ ${makedepends[*]} == 'check git meson python python-libevdev python-pyudev python-yaml' ]]
[[ ${checkdepends[*]} == python-pytest && ${validpgpkeys[*]} == upstream-key ]]
[[ ${#source[@]} == 2 && ${#sha256sums[@]} == 2 && ${sha256sums[0]} == upstream-checksum ]]
arch-meson() { printf 'meson:%s\\n' "$*" >>"$TEST_LOG"; }
meson() { printf 'compile:%s\\n' "$*" >>"$TEST_LOG"; }
install() { [[ $1 == -Dm644 && $3 == -t ]]; mkdir -p "$4"; cp "$2" "$4/"; }
build
check
cd "$srcdir"
package_libinput-tools
[[ -f $pkgdir/usr/bin/libinput && -f $pkgdir/usr/lib/libinput/libinput-debug-events ]]
[[ -f $pkgdir/usr/lib/libinput/libinput-list-devices && ! -f $pkgdir/usr/lib/libinput.so.10 ]]
[[ ! -f $pkgdir/usr/lib/libinput/libinput-debug-gui ]]
[[ ${depends[*]} == *' libinput '* && $epoch == 1 ]]
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log = (self.root / "calls").read_text()
        self.assertIn("-D debug-gui=false", log)
        self.assertIn("check:libinput-tools", log)
        patch = (path / "cli.patch").read_bytes()
        self.assertIn(hashlib.sha256(patch).hexdigest(), (path / "PKGBUILD").read_text())
        self.assertEqual(list((path / "pkg").iterdir()), [path / "pkg/libinput-tools"])

    def test_changed_patch_anchor_cannot_build_untrimmed_tools(self):
        self.remote["PKGBUILD"] = RECIPE.replace("-D documentation=false", "-D documentation=true").encode()
        path = self.candidate().parent / "recipe"
        result = self.source(path, 'startdir=$PWD; source ./PKGBUILD')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed", result.stderr.lower())

    def test_tools_dependency_is_unversioned_while_building(self):
        # The pair is checked by the contract, with pkgbuilds/libinput's library
        # installed; a versioned dependency could not be met in the build root.
        self.remote["PKGBUILD"] = RECIPE.replace("pkgrel=1\n", "pkgrel=1\nepoch=2\n").encode()
        self.remote[".SRCINFO"] = self.info().replace(b"\tpkgrel = 1\n", b"\tpkgrel = 1\n\tepoch = 2\n")
        lock = u.prepare_candidate("libinput-tools", self.baseline, "extra", "2:1.32.0-1",
                                   self.state, stable_version="2:1.32.0-1", fetch=self.fetch)
        self.assertEqual(json.loads(lock.read_bytes())["package_version"], "3:1.32.0-1")
        result = self.source(lock.parent / "recipe", '''
startdir=$PWD
source ./PKGBUILD
[[ $epoch == 3 && ${depends[*]} == *' libinput '* && ${depends[*]} != *libinput=* ]]
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_checksums_or_split_function_fail_closed(self):
        for recipe, expected in ((RECIPE.replace("sha256sums=('upstream-checksum')", ""), "checksums"),
                                 (RECIPE.replace("check()", "removed_check()"), "check() is missing")):
            self.remote["PKGBUILD"] = recipe.encode()
            result = self.source(self.candidate().parent / "recipe", 'startdir=$PWD; source ./PKGBUILD')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected, result.stderr)

    def test_only_a_locked_candidate_has_a_pkgbuild(self):
        self.assertFalse((self.baseline / "PKGBUILD").exists())
        self.assertTrue((self.baseline / "upstream.json").is_file())
        self.assertTrue((self.candidate().parent / "recipe/PKGBUILD").is_file())
        self.assertFalse((self.baseline / "PKGBUILD").exists())


if __name__ == "__main__":
    unittest.main()
